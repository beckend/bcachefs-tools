# Multithreaded extent-level compression

## What this does

When a write to a bcachefs filesystem produces more than one chunk of
`encoded_extent_max` bytes and the filesystem is configured for zstd
compression, the chunks are compressed concurrently across a pool of
pre-allocated workers instead of one at a time.

Only compression runs in parallel.  Encrypt, checksum, and keylist
append remain serial.  This preserves all write-path invariants (nonce
monotonicity, position ordering, keylist sortedness) without changing
the on-disk format.

**zstd and gzip.** LZ4 continues to use the serial path because
its per-chunk latency is too low for parallelism to help (benchmark
shows 0.67x with 4 workers — overhead exceeds gain).  The workqueue
infrastructure itself supports all three algorithms.  gzip benefits
significantly (3.37x speedup with 4 workers on the benchmark
pattern).  `bch2_write_should_mt_compress()` gates the parallel write
path to exclude `BCH_COMPRESSION_OPT_lz4` only.

## Why this approach

### Why C-side, not Rust-side

The Rust binary's `current` TLS is NULL — it does not implement the
kernel task abstraction.  Any kernel code that touches `current` (which
is most of the write path) crashes.  The C side under `fs/` is the
canonical source for bcachefs and compiles into both the DKMS kernel
module and the userspace binary.

### Why serial prefix / parallel body / serial post-pass

The write-path loop at `bch2_write_extent` mutates shared state per
iteration: `op->nonce`, `op->pos.offset`, `op->version`,
`op->insert_keys`, `wp->sectors_free`, and the `src` bio iterator.
Most of these are not safe to advance concurrently.

Two observations make parallelism tractable:

1. **`uncompressed_size` is known before compression.** The chunk size
   is determined by clamping to `encoded_extent_max` and rounding to
   block size.  Nonce advance (`src_len >> 9`) and position advance
   (`src_len >> 9`) depend only on this pre-compression size, not on
   the compressed output.

2. **Closures are already used for fan-in.** `fs/btree/node_scan.c`
   dispatches N reads with `closure_get`/`closure_sync_unbounded`.  The
   same pattern works for compression workers.

So identifiers (nonce, position, version) are pre-computed in a serial
prefix, compression runs in parallel, then encrypt/checksum/keylist
append run in a serial post-pass that consumes results in submission
order.

### Why not parallelise the move path

The move path (`BCH_WRITE_data_encoded`) has a pre-existing CRC with
`op->nonce` advance on the encoded data.  The nonce depends on the
compressed output size, which is only known after compression.  This
creates a circular dependency that cannot be resolved without changing
the on-disk format.  The move path is excluded by the
`BCH_WRITE_data_encoded` check in `bch2_write_should_mt_compress()`.

### Why pre-allocated per-worker buffers instead of mempool

Each worker has its own `workspace`, `dst_buf`, and `verify_buf`
allocated at mount time.  This avoids mempool contention on the
compression hot path — workers never block waiting for a workspace.

The alternative (allocate from mempools per-work) was rejected because:
- Mempool allocation can sleep under memory pressure, stalling the WQ.
- The existing mempools were sized for serial use (`min_nr=1`); scaling
  them up is equivalent to pre-allocating anyway.

## Architecture

### Worker pool (`fs/data/compress_workers.h`, `fs/data/compress_workers.c`)

```
struct bch_compress_wq {
    struct workqueue_struct  *wq;          // kernel: real cmwq; userspace: shim
    struct bch_compress_worker *workers;   // nr_workers entries
    unsigned                 nr_workers;   // clamp(num_online_cpus(), 1, 32)
    struct bch_fs           *c;            // backpointer for bch2_compress_locked
};

struct bch_compress_worker {
    void *workspace;    // zstd CCtx, sized for max level
    void *dst_buf;      // output buffer, sized for encoded_extent_max
    void *verify_buf;   // verify buffer, sized for encoded_extent_max
};
```

Created in `bch2_fs_compress_init()` after mempool setup.  If
allocation fails, `mt_wq` stays NULL and the parallel path does not
engage — the serial loop handles everything.  No mount failure.

`bch2_compress_wq_submit()` initializes a `struct bch_compress_work`,
calls `closure_get(parent)` then `queue_work(wq, &work->work)`.
The worker function calls `bch2_compress_locked()` then
`closure_put(parent)`.  The submitter calls `closure_sync_unbounded()`
to wait for all workers.

### Extracted `bch2_compress_locked` (`fs/data/compress.c`)

`bch2_compress()` was split into:

- **`bch2_compress_locked()`** — takes explicit workspace, dst, and
  verify_buf pointers.  Called by workers with pre-allocated buffers.
  Has a `bool may_shrink` parameter: when `false`, the shrink-retry
  loop is skipped — if the first compression attempt produces output
  larger than `dst_len`, the function returns incompressible
  immediately instead of trying smaller `src_len` values.

- **`bch2_compress()`** — thin wrapper that allocates workspace and
  verify_buf from mempools, calls `bch2_compress_locked(..., true)`,
  then frees them.  Used by the serial path.

**Why `may_shrink=false` for MT:** The serial path's shrink loop can
reduce `src_len` to find a size that compresses successfully.  In the
MT path, nonces and positions are pre-assigned based on the original
`chunk_src_len`.  If `src_len` shrinks, the source bio is not fully
consumed — the remaining bytes would be re-processed by the serial
fallback, causing double-processing and logical offset gaps.  Returning
incompressible is safe: the data is stored uncompressed (same as if
compression failed), and the serial path does not re-process it.

### MT write path (`fs/data/write.c`)

`bch2_write_extent_mt()` is called from `bch2_write_extent()` when
`bch2_write_should_mt_compress()` returns true.  The function:

1. **Serial prefix — chunk partitioning and identifier assignment:**
   - Partitions `src` bio into `batch` chunks (capped to
     `nr_workers`).
   - Copies each chunk to a private `src_bufs[i]` via `memcpy_from_bio`.
   - Assigns `chunk_pos[i] = op->pos`, advances `op->pos.offset`.
   - Assigns `nonces[i] = op->nonce`, advances `op->nonce`.
   - Assigns `versions[i]` (either `op->version` or
     `atomic64_inc_return(&c->key_version)` for zero-version).
   - All arrays are heap-allocated in `struct bch_write_mt_ctx` to
     avoid ~6 KB of stack usage.

2. **Parallel body — WQ dispatch:**
   - For each chunk, submits work to `workers[i % nr_workers]`.
   - Since `batch <= nr_workers`, each worker is used at most once —
     no `dst_buf`/`workspace` aliasing.
   - `closure_sync_unbounded()` waits for all workers to complete.

3. **Serial post-pass — encrypt, checksum, keylist append:**
   - For each chunk in submission order:
     - Reads compression result (`compression_type`, `dst_len`,
       `src_len`).
     - Constructs CRC (`compressed_size`, `uncompressed_size`,
       `nonce`).
     - Copies compressed (or original) data to `dst` bio.
     - Calls `bch2_encrypt_bio()` then `bch2_checksum_bio()`.
     - Calls `init_append_extent()` to append to `op->insert_keys`.
   - Advances `src` bio by `total_input = sum(chunk_src_len[i])`.
   - Advances `op->nonce` by `sum(chunk_src_len[i]) >> 9`.
   - Returns `total_output` (bytes written to `dst` bio).

**Error handling:**
- If encryption fails at chunk `i`, `op->insert_keys.top_p` is
  restored to `saved_keys_top` (captured before the post-pass loop),
  discarding any keys already appended.  `op->nonce` and
  `op->pos.offset` are also restored.
- The caller refreshes its own `saved_keys_top` after an MT error to
  handle potential keylist reallocation during the chunking loop.

### Gating conditions

`bch2_write_should_mt_compress()` returns true only when all of:

- `op->compression_opt != 0`
- Compression type is zstd (`co.type == BCH_COMPRESSION_OPT_zstd`)
- Not the encoded/move path (`!(op->flags & BCH_WRITE_data_encoded)`)
- Not marked incompressible (`!op->incompressible`)
- `c->compress.mt_wq != NULL` (pool initialised)
- `bch2_compress_nr_workers() > 1`
- `src->bi_iter.bi_size > encoded_extent_max` (more than one chunk)

### Worker count

`bch2_compress_nr_workers()` returns `max(bch2_compress_workers, 1)`.
When `bch2_compress_workers` is 0 (default), it uses
`num_online_cpus()` (via `sysconf(_SC_NPROCESSORS_ONLN)` in
userspace, or the kernel's real implementation in the module).

### `num_online_cpus()` fix (`linux/percpu.c`, `include/linux/cpumask.h`)

The userspace shim's `num_online_cpus()` was hard-coded to 1.  Changed
to call `sysconf(_SC_NPROCESSORS_ONLN)` cached on first use.  This
allows the compression pool to create more than one worker in the
userspace binary.

### Workqueue shim improvements (`linux/workqueue.c`)

Multi-worker support added: per-WQ worker task array, per-worker
condition variable, `active_count` tracking, `drain_workqueue` waits
for active + pending to drain.  The shim still creates workers lazily
(one on first `queue_work`), but now supports up to `max_active`
concurrent workers.

### Mempool sizing (`fs/data/compress.c`)

Workspace and bounce mempool reserves scaled from `min_nr=1` to
`bch2_compress_nr_workers()` so each concurrent worker has a reserved
workspace under memory pressure.

`zstd_workspace_size` is computed once with `WRITE_ONCE` from
`zstd_cctx_workspace_bound(zstd_max_clevel(), encoded_extent_max)`.
Read lock-free on the hot path with `READ_ONCE`.

## Test infrastructure

### In-kernel tests (`fs/debug/compress_test.c`, 37 tests)

Gated by `CONFIG_BCACHEFS_TESTS`.  Dispatched individually by name via
sysfs: `echo 'test_name count' > /sys/fs/bcachefs/*/compress_test`.

**MT zstd core (15 tests):**
- `test_mt_concurrency` — submits 8 work items with 10 ms sleep,
  verifies overlapping intervals.  Proves actual parallelism.
- `test_mt_nonce_uniqueness`, `test_mt_pos_monotonic`,
  `test_mt_dst_len_valid`, `test_mt_level_consistency`,
  `test_mt_worker_isolation`, `test_mt_batch_sizes` — invariant tests.
- `test_mt_compress_decompress`, `test_mt_compress_incompressible`,
  `test_mt_levels` — roundtrip, incompressible detection, all 15 zstd
  levels.
- `test_mt_mixed_compression`, `test_mt_single_chunk`,
  `test_mt_alignment`, `test_mt_empty_input`, `test_mt_reuse` — edge
  cases.

**Corruption handling (10 tests):**
- `test_corrupt_zstd_single_byte`, `test_corrupt_zstd_header_len`,
  `test_corrupt_zstd_header_zero`, `test_corrupt_all_ones`,
  `test_corrupt_first_byte`, `test_corrupt_last_byte`,
  `test_corrupt_crc_compressed_size`, `test_corrupt_crc_uncompressed_size`,
  `test_corrupt_recompress_after`, `test_corrupt_crc_wrong_type`.

**Serial LZ4/gzip regression (6 tests):**
- `test_lz4_roundtrip`, `test_lz4_incompressible`, `test_lz4_levels`.
- `test_gzip_roundtrip`, `test_gzip_incompressible`, `test_gzip_levels`.

**MT WQ LZ4 (3 tests):**
- `test_mt_lz4_roundtrip`, `test_mt_lz4_incompressible`,
  `test_mt_lz4_levels`.

**MT WQ gzip (3 tests):**
- `test_mt_gzip_roundtrip`, `test_mt_gzip_incompressible`,
  `test_mt_gzip_levels`.

### Micro-benchmark (`fs/debug/mt_compress_bench.c`)

- `mt_compress_bench` sysfs — serial vs parallel timing for a given
  compression opt.  Reports `speedup` as a ratio.  Informational only
  (always reports PASS).
- `mt_compress_bench_scaling` sysfs — sweep across
  `{1, 2, 4, 8, 16, 32}` effective workers, reporting per-row
  `serial_ns`, `parallel_ns`, `speedup_x100`, `throughput_mib_s_x100`.

### NixOS VM test (`nixos-test.nix`, 19 subtests)

Runs in a 4 vCPU QEMU VM with `CONFIG_BCACHEFS_TESTS=y`:

1. Module built from local sources
2. MT compress workers initialized
3. MT compression roundtrip (128 MiB compressible, ratio verified)
4. MT compression workers concurrent (overlapping intervals confirmed)
5. MT invariant verification (9 in-kernel tests via sysfs)
6. MT edge cases (5 in-kernel tests via sysfs)
7. MT corruption handling (10 in-kernel tests via sysfs)
8. MT compression ratio (256 MiB, ratio < 0.02)
9. MT stress test (5 format + write + fsck cycles)
10. MT small write fallback (4 KiB below MT threshold)
11. LZ4 compression regression (3 serial in-kernel tests)
12. Gzip compression regression (3 serial in-kernel tests)
13. MT compress benchmark (serial vs parallel timing)
14. MT compress benchmark scaling (per-row metrics)
15. MT LZ4 compression (3 WQ in-kernel tests, lz4 filesystem)
16. MT gzip compression (3 WQ in-kernel tests, gzip filesystem)

## On-disk format

Unchanged.  The MT path produces identical compressed extents: same zstd
stream, same CRC, same nonce derivation via `extent_nonce()`.  Existing
compressed data reads fine.

## Files changed

| File | What |
|------|------|
| `fs/data/compress.c` | `bch2_compress_locked` extraction, `may_shrink` param, mempool scaling, `zstd_workspace_size` write-once |
| `fs/data/compress.h` | Exports `bch2_compress_locked`, `buf_uncompress`, `bch2_compress_nr_workers` |
| `fs/data/compress_types.h` | `struct bch_fs_compress` gains `mt_wq` pointer, documented `zstd_workspace_size` field |
| `fs/data/compress_workers.c` | New: worker pool init/destroy, `bch_compress_work_fn`, `bch2_compress_wq_submit` |
| `fs/data/compress_workers.h` | New: `bch_compress_wq`, `bch_compress_worker`, `bch_compress_work` structs and API |
| `fs/data/write.c` | `bch2_write_extent_mt()`, `bch2_write_should_mt_compress()`, `bch_write_mt_ctx` heap struct, caller integration |
| `fs/debug/compress_test.c` | New: 37 in-kernel tests |
| `fs/debug/mt_compress_bench.c` | New: micro-benchmark + scaling sweep |
| `fs/debug/sysfs.c` | New sysfs attributes: `compress_test`, `mt_compress_bench`, `mt_compress_bench_scaling` |
| `fs/debug/tests.h` | Declares test/bench functions |
| `fs/Makefile` | Adds new object files |
| `linux/workqueue.c` | Multi-worker shim: worker task array, per-worker condvar, active_count tracking |
| `linux/mempool.c` | `mempool_alloc_noprof` rename made explicit |
| `linux/percpu.c` | `num_online_cpus()` uses `sysconf` instead of hardcoded 1 |
| `include/linux/cpumask.h` | `num_online_cpus()` declaration updated |
| `nixos-test.nix` | 19 subtests covering all test categories |
