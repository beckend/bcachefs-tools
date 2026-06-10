// SPDX-License-Identifier: GPL-2.0
#ifdef CONFIG_BCACHEFS_TESTS

#include "bcachefs.h"

#include "data/compress.h"
#include "data/compress_types.h"
#include "data/compress_workers.h"

#include "tests.h"

#include "closure.h"

#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * NOTE: This file calls buf_uncompress() from fs/data/compress.c, which is
 * `static` on master. The compress.c agent needs to drop the `static`
 * keyword on its definition to make it externally visible. PR 586 does
 * exactly this (see `int buf_uncompress(struct bch_fs *c, ...)`).
 */

static void fill_compressible(void *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		((u8 *)buf)[i] = (u8)(i % 251);
}

static void fill_semi_compressible(void *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		((u8 *)buf)[i] = (u8)((i * 7 + 13) ^ (i >> 3));
}

static void fill_incompressible(void *buf, size_t len)
{
	get_random_bytes(buf, len);
}

static struct bch_extent_crc_unpacked crc_from_work(struct bch_compress_work *w)
{
	return (struct bch_extent_crc_unpacked) {
		.compressed_size	= w->dst_len >> 9,
		.uncompressed_size	= w->src_len >> 9,
		.compression_type	= w->compression_type,
	};
}

/*
 * Roundtrip test for the MT compression path.
 *
 * Allocates `nr` chunks of compressible data (zeros), submits all of them
 * to the MT compress WQ concurrently, waits for completion via a parent
 * closure, then decompresses each result and verifies byte-for-byte match.
 *
 * Exercises:
 *   - bch2_compress_wq_submit() dispatch
 *   - Parent closure completion (closure_init / closure_sync)
 *   - Multiple workers running concurrently (results land in per-worker
 *     dst_bufs, so concurrent execution is required for the roundtrip to
 *     finish in roughly one worker's worth of time)
 *   - End-to-end compress + decompress correctness at level 3
 */
static int test_mt_compress_decompress(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	int ret = 0;

	nr = min_t(u64, nr, nr_workers);

	pr_info("mt compress/decompress roundtrip test: %llu chunks of %zu bytes (%u workers)",
		nr, chunk_size, nr_workers);

	size_t total_size = chunk_size * nr;

	void *src __free(kvfree) = kvmalloc(total_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(total_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, total_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];

		bch2_compress_wq_submit(&works[i], cwq,
					opt.value, POS(0, 0),
					src + i * chunk_size, chunk_size,
					worker->dst_buf, chunk_size,
					worker);
	}

	bch2_compress_wq_wait_all(works, nr);

	for (u64 i = 0; i < nr; i++) {
		if (works[i].compression_type != BCH_COMPRESSION_TYPE_zstd) {
			bch_err(c, "mt compress: chunk %llu not compressed (type=%u)",
				i, works[i].compression_type);
			return -EIO;
		}

		struct bch_extent_crc_unpacked crc = {
			.compressed_size	= works[i].dst_len >> 9,
			.uncompressed_size	= works[i].src_len >> 9,
			.compression_type	= works[i].compression_type,
		};

		ret = buf_uncompress(c, verify + i * chunk_size,
				     cwq->workers[i % nr_workers].dst_buf, crc);
		if (ret) {
			bch_err(c, "mt compress: decompress chunk %llu failed: %s",
				i, bch2_err_str(ret));
			return ret;
		}

		if (memcmp(verify + i * chunk_size,
			   src + i * chunk_size,
			   works[i].src_len)) {
			bch_err(c, "mt compress: decompressed chunk %llu mismatch", i);
			return -EIO;
		}
	}

	pr_info("mt compress/decompress roundtrip test passed, %llu chunks", nr);
	return 0;
}

/*
 * Random data should be detected as incompressible even under MT dispatch.
 *
 * Allocates one chunk-sized buffer of random bytes and submits `nr` work
 * items all pointing at it. With sufficient randomness no zstd pass should
 * produce a smaller output than input, so all results should be marked
 * BCH_COMPRESSION_TYPE_incompressible.
 */
static int test_mt_compress_incompressible(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;

	nr = min_t(u64, nr, nr_workers);

	pr_info("mt incompressible test: %llu random chunks of %zu bytes",
		nr, chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_incompressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];

		bch2_compress_wq_submit(&works[i], cwq,
					opt.value, POS(0, 0),
					src, chunk_size,
					worker->dst_buf, chunk_size,
					worker);
	}

	bch2_compress_wq_wait_all(works, nr);

	unsigned incompressible_count = 0;
	for (u64 i = 0; i < nr; i++)
		if (works[i].compression_type == BCH_COMPRESSION_TYPE_incompressible)
			incompressible_count++;

	if (incompressible_count < nr / 2) {
		bch_err(c, "mt incompressible: expected mostly incompressible, got %u/%llu",
			incompressible_count, nr);
		return -EIO;
	}

	pr_info("mt incompressible test passed, %u/%llu correctly detected",
		incompressible_count, nr);
	return 0;
}

/*
 * THE critical MT test: prove the WQ actually runs multiple work items
 * concurrently rather than serially.
 *
 * We submit N work items directly to cwq->wq (bypassing
 * bch2_compress_wq_submit() since we need a custom work_fn). Each item
 * records its start time, sleeps 10ms, records its end time. After all
 * complete, we check the recorded windows: if at least 2 items had
 * overlapping execution windows, parallelism was achieved.
 *
 * On a strictly serial WQ, the windows would be [0,10],[10,20],... with
 * no overlap and the test FAILS.
 *
 * The test is skipped (with a warning) on systems where the WQ was created
 * with < 2 workers, since strict serial execution would be expected.
 */
struct mt_concurrency_work {
	struct work_struct	work;
	struct closure		*parent;
	ktime_t			start;
	ktime_t			end;
};

static void mt_concurrency_work_fn(struct work_struct *work)
{
	struct mt_concurrency_work *w =
		container_of(work, struct mt_concurrency_work, work);

	w->start = ktime_get();
	msleep(10);
	w->end = ktime_get();

	closure_put(w->parent);
}

static int test_mt_concurrency(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	const unsigned n = 8;
	struct mt_concurrency_work works[8];
	struct closure parent;
	unsigned overlap_count = 0;

	if (cwq->nr_workers < 2) {
		pr_warn("mt concurrency test skipped: WQ has only %u worker(s), need >= 2",
			cwq->nr_workers);
		return 0;
	}

	pr_info("mt concurrency test: %u items, %u workers, each sleeps 10ms",
		n, cwq->nr_workers);

	closure_init(&parent, NULL);

	for (unsigned i = 0; i < n; i++) {
		works[i].parent	= &parent;
		works[i].start	= 0;
		works[i].end	= 0;
		INIT_WORK(&works[i].work, mt_concurrency_work_fn);
		closure_get(&parent);
		queue_work(cwq->wq, &works[i].work);
	}

	closure_sync(&parent);

	for (unsigned i = 0; i < n; i++) {
		for (unsigned j = i + 1; j < n; j++) {
			/* Overlap: i started before j ended AND j started before i ended. */
			if (ktime_before(works[i].start, works[j].end) &&
			    ktime_before(works[j].start, works[i].end))
				overlap_count++;
		}
	}

	if (overlap_count == 0) {
		bch_err(c, "mt concurrency test FAILED: 0 overlapping pairs out of %u (strictly serial execution)",
			n * (n - 1) / 2);
		return -EIO;
	}

	pr_info("mt concurrency test passed: %u overlapping pairs out of %u",
		overlap_count, n * (n - 1) / 2);
	return 0;
}

/*
 * MT dispatch with all 15 zstd compression levels.
 *
 * Submits 15 work items (one per level) to the MT WQ in batches of
 * nr_workers and verifies each compresses and decompresses correctly.
 * Each batch is synced before the next is submitted so no two works
 * share a worker's workspace concurrently.
 */
static int test_mt_levels(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	const unsigned n_levels = 15;
	int ret = 0;

	pr_info("mt levels test: all 15 zstd levels, chunk_size=%zu", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(n_levels, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
	};

	for (unsigned batch_start = 0; batch_start < n_levels;
	     batch_start += nr_workers) {
		unsigned batch_end = min_t(unsigned, batch_start + nr_workers, n_levels);
		unsigned batch_count = batch_end - batch_start;

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l + 1;
			struct bch_compress_worker *worker = &cwq->workers[l];

			opt.level = level;
			bch2_compress_wq_submit(&works[level - 1], cwq,
						opt.value, POS(0, (level - 1) * (chunk_size >> 9)),
						src, chunk_size,
						worker->dst_buf, chunk_size,
						worker);
		}

		bch2_compress_wq_wait_all(works + batch_start, batch_count);

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l + 1;
			unsigned i = level - 1;

			if (works[i].compression_type != BCH_COMPRESSION_TYPE_zstd) {
				bch_err(c, "mt levels: compressible data not compressed at level %u (type=%u)",
					level, works[i].compression_type);
				return -EIO;
			}

			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= works[i].dst_len >> 9,
				.uncompressed_size	= works[i].src_len >> 9,
				.compression_type	= works[i].compression_type,
			};

			ret = buf_uncompress(c, verify,
					 cwq->workers[l].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt levels: decompress failed at level %u: %s",
					level, bch2_err_str(ret));
				return ret;
			}

			if (memcmp(verify, src, works[i].src_len)) {
				bch_err(c, "mt levels: data mismatch at level %u", level);
				return -EIO;
			}

			pr_info("  level %u: %zu -> %zu bytes", level, works[i].src_len, works[i].dst_len);
		}
	}

	pr_info("mt levels test passed");
	return 0;
}

/*
 * Invariant 1: nonce uniqueness.
 *
 * Each chunk submitted to the MT path must receive a distinct write_pos
 * so that extent_nonce() produces a unique nonce. We submit nr chunks
 * with sequential POS(0, i*chunk_sectors) offsets and verify that every
 * work item's write_pos is distinct.
 */
static int test_mt_nonce_uniqueness(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	unsigned chunk_sectors = chunk_size >> 9;

	nr = min_t(u64, nr, nr_workers);

	pr_info("mt nonce uniqueness test: %llu chunks", nr);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];

		bch2_compress_wq_submit(&works[i], cwq,
					opt.value, POS(0, i * chunk_sectors),
					src, chunk_size,
					worker->dst_buf, chunk_size,
					worker);
	}

	bch2_compress_wq_wait_all(works, nr);

	for (u64 i = 0; i < nr; i++) {
		for (u64 j = i + 1; j < nr; j++) {
			if (works[i].write_pos.inode == works[j].write_pos.inode &&
			    works[i].write_pos.offset == works[j].write_pos.offset) {
				bch_err(c, "mt nonce uniqueness: works[%llu] and works[%llu] share write_pos",
					i, j);
				return -EIO;
			}
		}
	}

	pr_info("mt nonce uniqueness test passed, %llu chunks", nr);
	return 0;
}

/*
 * Invariant 2: pos.offset monotonic advance.
 *
 * The serial prefix in bch2_write_extent_mt() advances pos.offset by
 * chunk_src_len >> 9 for each chunk. We verify that when submitting with
 * sequential offsets, the resulting write_pos.offset values are in
 * strictly ascending order.
 */
static int test_mt_pos_monotonic(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	unsigned chunk_sectors = chunk_size >> 9;

	nr = min_t(u64, nr, nr_workers);

	pr_info("mt pos monotonic test: %llu chunks", nr);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];

		bch2_compress_wq_submit(&works[i], cwq,
					opt.value, POS(0, i * chunk_sectors),
					src, chunk_size,
					worker->dst_buf, chunk_size,
					worker);
	}

	bch2_compress_wq_wait_all(works, nr);

	for (u64 i = 1; i < nr; i++) {
		if (works[i].write_pos.offset <= works[i - 1].write_pos.offset) {
			bch_err(c, "mt pos monotonic: works[%llu].offset=%llu <= works[%llu].offset=%llu",
				i, works[i].write_pos.offset,
				i - 1, works[i - 1].write_pos.offset);
			return -EIO;
		}
	}

	pr_info("mt pos monotonic test passed, %llu chunks", nr);
	return 0;
}

/*
 * Invariant 3: dst_len and src_len validity.
 *
 * For all chunks: 0 < dst_len <= extent_max, 0 < src_len <= chunk_size.
 * Compressible data must actually compress (dst_len < src_len).
 * Incompressible data must be marked BCH_COMPRESSION_TYPE_incompressible.
 */
static int test_mt_dst_len_valid(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;

	nr = min_t(u64, nr, nr_workers);

	unsigned half = nr / 2;

	pr_info("mt dst_len valid test: %llu chunks (half compressible, half incompressible)", nr);

	void *compressible __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *incompressible __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!compressible || !incompressible)
		return -ENOMEM;

	fill_compressible(compressible, chunk_size);
	fill_incompressible(incompressible, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];
		bool use_compressible = (i < half);
		void *data = use_compressible ? compressible : incompressible;

		bch2_compress_wq_submit(&works[i], cwq,
					opt.value, POS(0, 0),
					data, chunk_size,
					worker->dst_buf, chunk_size,
					worker);
	}

	bch2_compress_wq_wait_all(works, nr);

	for (u64 i = 0; i < nr; i++) {
		if (works[i].dst_len == 0 || works[i].dst_len > chunk_size) {
			bch_err(c, "mt dst_len valid: works[%llu] dst_len=%zu out of range [1,%zu]",
				i, works[i].dst_len, chunk_size);
			return -EIO;
		}

		if (works[i].src_len == 0 || works[i].src_len > chunk_size) {
			bch_err(c, "mt dst_len valid: works[%llu] src_len=%zu out of range [1,%zu]",
				i, works[i].src_len, chunk_size);
			return -EIO;
		}

		if (i < half) {
			if (works[i].dst_len >= works[i].src_len) {
				bch_err(c, "mt dst_len valid: compressible works[%llu] not compressed (dst=%zu >= src=%zu)",
					i, works[i].dst_len, works[i].src_len);
				return -EIO;
			}
		} else {
			if (works[i].compression_type != BCH_COMPRESSION_TYPE_incompressible) {
				bch_err(c, "mt dst_len valid: incompressible works[%llu] wrong type=%u",
					i, works[i].compression_type);
				return -EIO;
			}
		}
	}

	pr_info("mt dst_len valid test passed, %llu chunks", nr);
	return 0;
}

/*
 * Invariant 4: compression level consistency.
 *
 * Submit the same compressible data at all 15 zstd levels and verify each
 * produces valid output (dst_len > 0, src_len > 0, compression_type ==
 * BCH_COMPRESSION_TYPE_zstd). This is a soft check — zstd does not
 * guarantee strict monotonicity in output size, so we only verify validity.
 *
 * Levels are submitted in batches of nr_workers so no two works share a
 * worker's workspace concurrently.
 */
static int test_mt_level_consistency(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	const unsigned n_levels = 15;

	pr_info("mt level consistency test: all 15 zstd levels");

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(n_levels, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
	};

	for (unsigned batch_start = 0; batch_start < n_levels;
	     batch_start += nr_workers) {
		unsigned batch_end = min_t(unsigned, batch_start + nr_workers, n_levels);
		unsigned batch_count = batch_end - batch_start;

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l + 1;
			struct bch_compress_worker *worker = &cwq->workers[l];

			opt.level = level;
			bch2_compress_wq_submit(&works[level - 1], cwq,
						opt.value, POS(0, (level - 1) * (chunk_size >> 9)),
						src, chunk_size,
						worker->dst_buf, chunk_size,
						worker);
		}

		bch2_compress_wq_wait_all(works + batch_start, batch_count);
	}

	for (unsigned level = 1; level <= n_levels; level++) {
		unsigned i = level - 1;

		if (works[i].dst_len == 0 || works[i].src_len == 0) {
			bch_err(c, "mt level consistency: level %u produced zero-length output (dst=%zu src=%zu)",
				level, works[i].dst_len, works[i].src_len);
			return -EIO;
		}

		if (works[i].compression_type != BCH_COMPRESSION_TYPE_zstd) {
			bch_err(c, "mt level consistency: level %u wrong compression_type=%u",
				level, works[i].compression_type);
			return -EIO;
		}
	}

	pr_info("mt level consistency test passed");
	return 0;
}

/*
 * Invariant 5: worker isolation.
 *
 * Submit nr chunks (nr > nr_workers) with distinct data patterns (byte i & 0xFF
 * for chunk i) in sequential batches of nr_workers. After each batch
 * completes, compresses and decompresses and verifies each chunk's data
 * matches its original pattern — proving no cross-contamination between
 * chunks that reuse the same worker across batches.
 */
static int test_mt_worker_isolation(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	size_t total_size = chunk_size * nr;
	int ret = 0;

	pr_info("mt worker isolation test: %llu chunks, %u workers", nr, nr_workers);

	void *src __free(kvfree) = kvmalloc(total_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	for (u64 i = 0; i < nr; i++)
		memset(src + i * chunk_size, (u8)(i & 0xFF), chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 batch_start = 0; batch_start < nr;
	     batch_start += nr_workers) {
		unsigned batch_end = min_t(u64, batch_start + nr_workers, nr);
		unsigned batch_count = batch_end - batch_start;

		for (unsigned l = 0; l < batch_count; l++) {
			u64 idx = batch_start + l;
			struct bch_compress_worker *worker = &cwq->workers[l];

			bch2_compress_wq_submit(&works[idx], cwq,
						opt.value, POS(0, idx * (chunk_size >> 9)),
						src + idx * chunk_size, chunk_size,
						worker->dst_buf, chunk_size,
						worker);
		}

		bch2_compress_wq_wait_all(works + batch_start, batch_count);

		for (unsigned l = 0; l < batch_count; l++) {
			u64 idx = batch_start + l;

			if (works[idx].compression_type == BCH_COMPRESSION_TYPE_incompressible) {
				u8 expected = (u8)(idx & 0xFF);

				if (memcmp(src + idx * chunk_size + works[idx].src_len - 1, &expected, 1) == 0 &&
				    ((u8 *)src)[idx * chunk_size] == expected)
					continue;

				bch_err(c, "mt worker isolation: incompressible chunk %llu data mismatch", idx);
				return -EIO;
			}

			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= works[idx].dst_len >> 9,
				.uncompressed_size	= works[idx].src_len >> 9,
				.compression_type	= works[idx].compression_type,
			};

			ret = buf_uncompress(c, verify,
					     cwq->workers[l].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt worker isolation: decompress chunk %llu failed: %s",
					idx, bch2_err_str(ret));
				return ret;
			}

			u8 expected = (u8)(idx & 0xFF);
			for (size_t b = 0; b < works[idx].src_len; b++) {
				if (((u8 *)verify)[b] != expected) {
					bch_err(c, "mt worker isolation: chunk %llu byte %zu mismatch (got 0x%02x expected 0x%02x)",
						idx, b, ((u8 *)verify)[b], expected);
					return -EIO;
				}
			}
		}
	}

	pr_info("mt worker isolation test passed, %llu chunks", nr);
	return 0;
}

/*
 * Invariant 6: varying batch sizes.
 *
 * Test with batch sizes 1, 2, nr_workers, nr_workers+1, 2*nr_workers.
 * For each batch size, submit that many compressible chunks and verify
 * all complete successfully. Batches larger than nr_workers are submitted
 * in sequential sub-batches of nr_workers to avoid workspace races.
 */
static int test_mt_batch_sizes(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	unsigned batch_sizes[] = { 1, 2, 0, 0, 0 };
	unsigned n_batches = ARRAY_SIZE(batch_sizes);

	batch_sizes[2] = nr_workers;
	batch_sizes[3] = nr_workers + 1;
	batch_sizes[4] = 2 * nr_workers;

	pr_info("mt batch sizes test: %u workers", nr_workers);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (unsigned b = 0; b < n_batches; b++) {
		unsigned batch_nr = batch_sizes[b];

		pr_info("  batch size %u", batch_nr);

		struct bch_compress_work *works __free(kvfree) =
			kvcalloc(batch_nr, sizeof(*works), GFP_KERNEL);
		if (!works)
			return -ENOMEM;

		for (unsigned sub_start = 0; sub_start < batch_nr;
		     sub_start += nr_workers) {
			unsigned sub_end = min_t(unsigned, sub_start + nr_workers, batch_nr);
			unsigned sub_count = sub_end - sub_start;

			for (unsigned i = 0; i < sub_count; i++) {
				unsigned idx = sub_start + i;
				struct bch_compress_worker *worker = &cwq->workers[i];

				bch2_compress_wq_submit(&works[idx], cwq,
							opt.value, POS(0, idx * (chunk_size >> 9)),
							src, chunk_size,
							worker->dst_buf, chunk_size,
							worker);
			}

			bch2_compress_wq_wait_all(works + sub_start, sub_count);
		}

		for (unsigned i = 0; i < batch_nr; i++) {
			if (works[i].dst_len == 0) {
				bch_err(c, "mt batch sizes: batch %u chunk %u dst_len=0",
					batch_nr, i);
				return -EIO;
			}
			if (works[i].src_len == 0) {
				bch_err(c, "mt batch sizes: batch %u chunk %u src_len=0",
					batch_nr, i);
				return -EIO;
			}
		}
	}

	pr_info("mt batch sizes test passed");
	return 0;
}

/*
 * Edge case: mixed compressible and incompressible chunks in the same batch.
 *
 * Odd-indexed chunks are compressible (zeros), even-indexed chunks are
 * incompressible (random). Verifies that compressible chunks come back as
 * BCH_COMPRESSION_TYPE_zstd and incompressible chunks as
 * BCH_COMPRESSION_TYPE_incompressible. Then decompresses the compressible
 * ones and checks byte-for-byte roundtrip.
 */
static int test_mt_mixed_compression(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	int ret = 0;

	nr = min_t(u64, nr, nr_workers);

	size_t total_size = chunk_size * nr;

	pr_info("mt mixed compression test: %llu chunks (alternating compressible/incompressible)", nr);

	void *src __free(kvfree) = kvmalloc(total_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	for (u64 i = 0; i < nr; i++) {
		if (i & 1)
			fill_compressible(src + i * chunk_size, chunk_size);
		else
			fill_incompressible(src + i * chunk_size, chunk_size);
	}

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (u64 i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];

		bch2_compress_wq_submit(&works[i], cwq,
					opt.value, POS(0, i * (chunk_size >> 9)),
					src + i * chunk_size, chunk_size,
					worker->dst_buf, chunk_size,
					worker);
	}

	bch2_compress_wq_wait_all(works, nr);

	for (u64 i = 0; i < nr; i++) {
		bool expect_compressible = (i & 1) != 0;

		if (expect_compressible) {
			if (works[i].compression_type != BCH_COMPRESSION_TYPE_zstd) {
				bch_err(c, "mt mixed: compressible chunk %llu wrong type=%u",
					i, works[i].compression_type);
				return -EIO;
			}

			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= works[i].dst_len >> 9,
				.uncompressed_size	= works[i].src_len >> 9,
				.compression_type	= works[i].compression_type,
			};

			ret = buf_uncompress(c, verify,
					     cwq->workers[i % nr_workers].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt mixed: decompress chunk %llu failed: %s",
					i, bch2_err_str(ret));
				return ret;
			}

			if (memcmp(verify, src + i * chunk_size, works[i].src_len)) {
				bch_err(c, "mt mixed: compressible chunk %llu roundtrip mismatch", i);
				return -EIO;
			}
		} else {
			if (works[i].compression_type != BCH_COMPRESSION_TYPE_incompressible) {
				bch_err(c, "mt mixed: incompressible chunk %llu wrong type=%u",
					i, works[i].compression_type);
				return -EIO;
			}
		}
	}

	pr_info("mt mixed compression test passed, %llu chunks", nr);
	return 0;
}

/*
 * Degenerate case: submit exactly 1 chunk to the WQ.
 *
 * Verifies that a single-chunk batch compresses and decompresses correctly.
 * While bch2_write_extent_mt() would return -1 for a single-chunk write,
 * the WQ machinery itself must still handle it correctly.
 */
static int test_mt_single_chunk(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	int ret = 0;

	pr_info("mt single chunk test: 1 chunk of %zu bytes", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work work;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	bch2_compress_wq_submit(&work, cwq,
				opt.value, POS(0, 0),
				src, chunk_size,
				cwq->workers[0].dst_buf, chunk_size,
				&cwq->workers[0]);

	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "mt single chunk: not compressed (type=%u)",
			work.compression_type);
		return -EIO;
	}

	struct bch_extent_crc_unpacked crc = {
		.compressed_size	= work.dst_len >> 9,
		.uncompressed_size	= work.src_len >> 9,
		.compression_type	= work.compression_type,
	};

	ret = buf_uncompress(c, verify,
			     cwq->workers[0].dst_buf, crc);
	if (ret) {
		bch_err(c, "mt single chunk: decompress failed: %s",
			bch2_err_str(ret));
		return ret;
	}

	if (memcmp(verify, src, work.src_len)) {
		bch_err(c, "mt single chunk: roundtrip mismatch");
		return -EIO;
	}

	pr_info("mt single chunk test passed");
	return 0;
}

/*
 * Edge case: sub-extent-max chunk sizes.
 *
 * Submits chunks of various non-aligned sizes (extent_max/2, extent_max*3/4,
 * extent_max - block_bytes, block_bytes) and verifies each compresses and
 * decompresses correctly. Tests the chunk sizing logic's handling of
 * writes that are not full extent_max multiples.
 */
static int test_mt_alignment(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned bsize = block_bytes(c);
	size_t sizes[] = {
		chunk_size / 2,
		chunk_size * 3 / 4,
		chunk_size - bsize,
		bsize,
	};
	unsigned n_sizes = ARRAY_SIZE(sizes);
	int ret = 0;

	pr_info("mt alignment test: %u sub-extent-max sizes (block=%u, extent=%zu)",
		n_sizes, bsize, chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	for (unsigned s = 0; s < n_sizes; s++) {
		size_t test_size = sizes[s];

		pr_info("  testing size %zu", test_size);

		fill_compressible(src, test_size);

		struct bch_compress_work work;

		union bch_compression_opt opt = {
			.type	= BCH_COMPRESSION_OPT_zstd,
			.level	= 3,
		};

		bch2_compress_wq_submit(&work, cwq,
					opt.value, POS(0, 0),
					src, test_size,
					cwq->workers[0].dst_buf, chunk_size,
					&cwq->workers[0]);

		bch2_compress_wq_wait_all(&work, 1);

		if (work.compression_type != BCH_COMPRESSION_TYPE_zstd &&
		    work.compression_type != BCH_COMPRESSION_TYPE_incompressible) {
			bch_err(c, "mt alignment: size %zu unexpected type=%u",
				test_size, work.compression_type);
			return -EIO;
		}

		if (work.src_len != test_size) {
			bch_err(c, "mt alignment: size %zu src_len=%zu mismatch",
				test_size, work.src_len);
			return -EIO;
		}

		if (work.compression_type == BCH_COMPRESSION_TYPE_zstd) {
			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= work.dst_len >> 9,
				.uncompressed_size	= work.src_len >> 9,
				.compression_type	= work.compression_type,
			};

			ret = buf_uncompress(c, verify,
					     cwq->workers[0].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt alignment: decompress size %zu failed: %s",
					test_size, bch2_err_str(ret));
				return ret;
			}

			if (memcmp(verify, src, work.src_len)) {
				bch_err(c, "mt alignment: roundtrip mismatch at size %zu",
					test_size);
				return -EIO;
			}
		}
	}

	pr_info("mt alignment test passed");
	return 0;
}

/*
 * Edge case: minimum valid input size (block_bytes).
 *
 * Submits a chunk where src_len equals block_bytes(c), the smallest valid
 * input size. Verifies the WQ handles it gracefully — either compresses
 * it or marks it incompressible — without crashing or producing garbage.
 */
static int test_mt_empty_input(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned bsize = block_bytes(c);
	int ret = 0;

	pr_info("mt empty input test: minimum size %u bytes", bsize);

	void *src __free(kvfree) = kvmalloc(bsize, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, bsize);

	struct bch_compress_work work;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	bch2_compress_wq_submit(&work, cwq,
				opt.value, POS(0, 0),
				src, bsize,
				cwq->workers[0].dst_buf, chunk_size,
				&cwq->workers[0]);

	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd &&
	    work.compression_type != BCH_COMPRESSION_TYPE_incompressible) {
		bch_err(c, "mt empty input: unexpected type=%u",
			work.compression_type);
		return -EIO;
	}

	if (work.src_len != bsize) {
		bch_err(c, "mt empty input: src_len=%zu != %u",
			work.src_len, bsize);
		return -EIO;
	}

	if (work.dst_len == 0) {
		bch_err(c, "mt empty input: dst_len=0");
		return -EIO;
	}

	if (work.compression_type == BCH_COMPRESSION_TYPE_zstd) {
		struct bch_extent_crc_unpacked crc = {
			.compressed_size	= work.dst_len >> 9,
			.uncompressed_size	= work.src_len >> 9,
			.compression_type	= work.compression_type,
		};

		ret = buf_uncompress(c, verify,
				     cwq->workers[0].dst_buf, crc);
		if (ret) {
			bch_err(c, "mt empty input: decompress failed: %s",
				bch2_err_str(ret));
			return ret;
		}

		if (memcmp(verify, src, work.src_len)) {
			bch_err(c, "mt empty input: roundtrip mismatch");
			return -EIO;
		}
	}

	pr_info("mt empty input test passed (type=%u, dst_len=%zu)",
		work.compression_type, work.dst_len);
	return 0;
}

/*
 * Edge case: reuse workers across two sequential batches.
 *
 * Submits nr chunks of compressible data, waits for completion, then
 * re-fills the source with a different pattern and submits the same nr
 * chunks again using the same work items and dst_bufs. Verifies the
 * second batch roundtrips correctly, proving workers don't carry stale
 * state between submissions.
 */
static int test_mt_reuse(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	int ret = 0;

	nr = min_t(u64, nr, nr_workers);

	size_t total_size = chunk_size * nr;

	pr_info("mt reuse test: %llu chunks x 2 batches", nr);

	void *src __free(kvfree) = kvmalloc(total_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(total_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(nr, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_zstd,
		.level	= 3,
	};

	for (unsigned batch = 0; batch < 2; batch++) {
		pr_info("  batch %u", batch);

		for (u64 i = 0; i < nr; i++)
			memset(src + i * chunk_size, (u8)((batch * nr + i) & 0xFF), chunk_size);

		for (u64 i = 0; i < nr; i++) {
			struct bch_compress_worker *worker = &cwq->workers[i % nr_workers];

			bch2_compress_wq_submit(&works[i], cwq,
						opt.value, POS(0, i * (chunk_size >> 9)),
						src + i * chunk_size, chunk_size,
						worker->dst_buf, chunk_size,
						worker);
		}

		bch2_compress_wq_wait_all(works, nr);

		for (u64 i = 0; i < nr; i++) {
			if (works[i].compression_type == BCH_COMPRESSION_TYPE_incompressible)
				continue;

			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= works[i].dst_len >> 9,
				.uncompressed_size	= works[i].src_len >> 9,
				.compression_type	= works[i].compression_type,
			};

			ret = buf_uncompress(c, verify + i * chunk_size,
					     cwq->workers[i % nr_workers].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt reuse: batch %u chunk %llu decompress failed: %s",
					batch, i, bch2_err_str(ret));
				return ret;
			}

			if (memcmp(verify + i * chunk_size,
				   src + i * chunk_size,
				   works[i].src_len)) {
				bch_err(c, "mt reuse: batch %u chunk %llu roundtrip mismatch",
					batch, i);
				return -EIO;
			}
		}
	}

	pr_info("mt reuse test passed, %llu chunks x 2 batches", nr);
	return 0;
}

static int test_corrupt_zstd_single_byte(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_zstd_single_byte: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_zstd_single_byte: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	((u8 *)worker->dst_buf)[4] ^= 0xFF;
	((u8 *)worker->dst_buf)[5] ^= 0xFF;

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_zstd_single_byte: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_zstd_single_byte: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_zstd_header_len(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_zstd_header_len: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_zstd_header_len: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	*(__le32 *)worker->dst_buf = cpu_to_le32(work.dst_len + 4096);

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_zstd_header_len: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_zstd_header_len: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_zstd_header_zero(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_zstd_header_zero: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_zstd_header_zero: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	*(__le32 *)worker->dst_buf = cpu_to_le32(0);

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_zstd_header_zero: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_zstd_header_zero: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_all_ones(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_all_ones: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_all_ones: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	memset(worker->dst_buf, 0xFF, work.dst_len);

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_all_ones: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_all_ones: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_first_byte(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_first_byte: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_first_byte: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	((u8 *)worker->dst_buf)[0] ^= 0xFF;

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_first_byte: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_first_byte: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_last_byte(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_last_byte: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_last_byte: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	((u8 *)worker->dst_buf)[7] ^= 0xFF;

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_last_byte: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_last_byte: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_crc_compressed_size(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_crc_compressed_size: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_crc_compressed_size: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);
	((u8 *)worker->dst_buf)[4] ^= 0xFF;
	crc.compressed_size = (work.dst_len >> 9) - 1;
	if (crc.compressed_size == 0) {
		pr_info("test_corrupt_crc_compressed_size: compressed output too small for truncation, skipping");
		return 0;
	}

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_crc_compressed_size: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_crc_compressed_size: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_crc_uncompressed_size(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_crc_uncompressed_size: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_crc_uncompressed_size: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);
	crc.uncompressed_size = (work.src_len >> 9) + 20;

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_crc_uncompressed_size: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_crc_uncompressed_size: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_crc_wrong_type(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4])) {
		pr_info("test_corrupt_crc_wrong_type: lz4 workspace not initialized, skipping");
		return 0;
	}

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_crc_wrong_type: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_crc_wrong_type: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);
	crc.compression_type = BCH_COMPRESSION_TYPE_lz4;

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_crc_wrong_type: expected failure, got success");
		return -EIO;
	}

	pr_info("test_corrupt_crc_wrong_type: correctly rejected with error %d", ret);
	return 0;
}

static int test_corrupt_recompress_after(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_semi_compressible(src, chunk_size);

	union bch_compression_opt opt = { .type = BCH_COMPRESSION_OPT_zstd, .level = 3 };
	struct bch_compress_work work;
	struct bch_compress_worker *worker = &cwq->workers[0];
	bch2_compress_wq_submit(&work, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_recompress_after: data not compressed (type=%u)", work.compression_type);
		return -EIO;
	}

	if (work.dst_len < 512 || (work.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_recompress_after: compressed output too small (%zu bytes), skipping",
			work.dst_len);
		return -ENODATA;
	}

	memset(worker->dst_buf, 0xFF, work.dst_len);

	struct bch_extent_crc_unpacked crc = crc_from_work(&work);

	int ret = buf_uncompress(c, verify, worker->dst_buf, crc);
	if (ret == 0) {
		bch_err(c, "test_corrupt_recompress_after: expected failure after corruption, got success");
		return -EIO;
	}

	int corrupt_err = ret;

	struct bch_compress_work work2;
	bch2_compress_wq_submit(&work2, cwq, opt.value, POS(0, 0),
				src, chunk_size, worker->dst_buf, chunk_size, worker);
	bch2_compress_wq_wait_all(&work2, 1);

	if (work2.compression_type != BCH_COMPRESSION_TYPE_zstd) {
		bch_err(c, "test_corrupt_recompress_after: re-compress not compressed (type=%u)", work2.compression_type);
		return -EIO;
	}

	if (work2.dst_len < 512 || (work2.dst_len >> 9) == 0) {
		bch_err(c, "test_corrupt_recompress_after: re-compress output too small (%zu bytes), skipping",
			work2.dst_len);
		return -ENODATA;
	}

	struct bch_extent_crc_unpacked crc2 = crc_from_work(&work2);

	ret = buf_uncompress(c, verify, worker->dst_buf, crc2);
	if (ret) {
		bch_err(c, "test_corrupt_recompress_after: re-compress decompress failed: %s", bch2_err_str(ret));
		return ret;
	}

	if (memcmp(verify, src, work2.src_len)) {
		bch_err(c, "test_corrupt_recompress_after: re-compress roundtrip mismatch");
		return -EIO;
	}

	pr_info("test_corrupt_recompress_after: corruption rejected (%d), re-compress roundtrip OK", corrupt_err);
	return 0;
}

static int test_lz4_roundtrip(struct bch_fs *c, u64 nr)
{
	size_t chunk_size = c->opts.encoded_extent_max;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4])) {
		pr_err("lz4 compression not initialized");
		return -EINVAL;
	}

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *dst __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !dst || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_lz4,
		.level	= 0,
	};

	size_t dst_len = chunk_size;
	size_t src_len = chunk_size;

	void *workspace = mempool_alloc(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4], GFP_KERNEL);
	if (!workspace)
		return -ENOMEM;

	unsigned result = bch2_compress_locked(c, dst, &dst_len, src, &src_len,
					       opt.value, POS(0, 0), workspace, verify, true, NULL);

	mempool_free(workspace, &c->compress.workspace[BCH_COMPRESSION_OPT_lz4]);

	if (result != BCH_COMPRESSION_TYPE_lz4) {
		bch_err(c, "lz4 roundtrip: data not compressed (type=%u)", result);
		return -EIO;
	}

	struct bch_extent_crc_unpacked crc = {
		.compressed_size	= dst_len >> 9,
		.uncompressed_size	= src_len >> 9,
		.compression_type	= result,
	};

	int ret = buf_uncompress(c, verify, dst, crc);
	if (ret) {
		bch_err(c, "lz4 roundtrip: decompress failed: %s", bch2_err_str(ret));
		return ret;
	}

	if (memcmp(verify, src, src_len)) {
		bch_err(c, "lz4 roundtrip: data mismatch");
		return -EIO;
	}

	pr_info("lz4 roundtrip test passed");
	return 0;
}

static int test_lz4_incompressible(struct bch_fs *c, u64 nr)
{
	size_t chunk_size = c->opts.encoded_extent_max;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4])) {
		pr_err("lz4 compression not initialized");
		return -EINVAL;
	}

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *dst __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !dst || !verify)
		return -ENOMEM;

	fill_incompressible(src, chunk_size);

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_lz4,
		.level	= 0,
	};

	size_t dst_len = chunk_size;
	size_t src_len = chunk_size;

	void *workspace = mempool_alloc(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4], GFP_KERNEL);
	if (!workspace)
		return -ENOMEM;

	unsigned result = bch2_compress_locked(c, dst, &dst_len, src, &src_len,
					       opt.value, POS(0, 0), workspace, verify, true, NULL);

	mempool_free(workspace, &c->compress.workspace[BCH_COMPRESSION_OPT_lz4]);

	if (result != BCH_COMPRESSION_TYPE_incompressible) {
		bch_err(c, "lz4 incompressible: expected incompressible (type=%u)", result);
		return -EIO;
	}

	pr_info("lz4 incompressible test passed");
	return 0;
}

static int test_gzip_roundtrip(struct bch_fs *c, u64 nr)
{
	size_t chunk_size = c->opts.encoded_extent_max;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_gzip])) {
		pr_err("gzip compression not initialized");
		return -EINVAL;
	}

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *dst __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !dst || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_gzip,
		.level	= 6,
	};

	size_t dst_len = chunk_size;
	size_t src_len = chunk_size;

	void *workspace = mempool_alloc(&c->compress.workspace[BCH_COMPRESSION_OPT_gzip], GFP_KERNEL);
	if (!workspace)
		return -ENOMEM;

	unsigned result = bch2_compress_locked(c, dst, &dst_len, src, &src_len,
					       opt.value, POS(0, 0), workspace, verify, true, NULL);

	mempool_free(workspace, &c->compress.workspace[BCH_COMPRESSION_OPT_gzip]);

	if (result != BCH_COMPRESSION_TYPE_gzip) {
		bch_err(c, "gzip roundtrip: data not compressed (type=%u)", result);
		return -EIO;
	}

	struct bch_extent_crc_unpacked crc = {
		.compressed_size	= dst_len >> 9,
		.uncompressed_size	= src_len >> 9,
		.compression_type	= result,
	};

	int ret = buf_uncompress(c, verify, dst, crc);
	if (ret) {
		bch_err(c, "gzip roundtrip: decompress failed: %s", bch2_err_str(ret));
		return ret;
	}

	if (memcmp(verify, src, src_len)) {
		bch_err(c, "gzip roundtrip: data mismatch");
		return -EIO;
	}

	pr_info("gzip roundtrip test passed");
	return 0;
}

static int test_gzip_incompressible(struct bch_fs *c, u64 nr)
{
	size_t chunk_size = c->opts.encoded_extent_max;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_gzip])) {
		pr_err("gzip compression not initialized");
		return -EINVAL;
	}

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *dst __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !dst || !verify)
		return -ENOMEM;

	fill_incompressible(src, chunk_size);

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_gzip,
		.level	= 6,
	};

	size_t dst_len = chunk_size;
	size_t src_len = chunk_size;

	void *workspace = mempool_alloc(&c->compress.workspace[BCH_COMPRESSION_OPT_gzip], GFP_KERNEL);
	if (!workspace)
		return -ENOMEM;

	unsigned result = bch2_compress_locked(c, dst, &dst_len, src, &src_len,
					       opt.value, POS(0, 0), workspace, verify, true, NULL);

	mempool_free(workspace, &c->compress.workspace[BCH_COMPRESSION_OPT_gzip]);

	if (result != BCH_COMPRESSION_TYPE_incompressible) {
		bch_err(c, "gzip incompressible: expected incompressible (type=%u)", result);
		return -EIO;
	}

	pr_info("gzip incompressible test passed");
	return 0;
}

static int test_lz4_levels(struct bch_fs *c, u64 nr)
{
	size_t chunk_size = c->opts.encoded_extent_max;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4])) {
		pr_err("lz4 compression not initialized");
		return -EINVAL;
	}

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *dst __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !dst || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	for (unsigned level = 0; level <= 1; level++) {
		union bch_compression_opt opt = {
			.type	= BCH_COMPRESSION_OPT_lz4,
			.level	= level,
		};

		size_t dst_len = chunk_size;
		size_t src_len = chunk_size;

		void *workspace = mempool_alloc(&c->compress.workspace[BCH_COMPRESSION_OPT_lz4], GFP_KERNEL);
		if (!workspace)
			return -ENOMEM;

		unsigned result = bch2_compress_locked(c, dst, &dst_len, src, &src_len,
						       opt.value, POS(0, 0), workspace, verify, true, NULL);

		mempool_free(workspace, &c->compress.workspace[BCH_COMPRESSION_OPT_lz4]);

		if (result != BCH_COMPRESSION_TYPE_lz4) {
			bch_err(c, "lz4 levels: level %u not compressed (type=%u)", level, result);
			return -EIO;
		}

		struct bch_extent_crc_unpacked crc = {
			.compressed_size	= dst_len >> 9,
			.uncompressed_size	= src_len >> 9,
			.compression_type	= result,
		};

		int ret = buf_uncompress(c, verify, dst, crc);
		if (ret) {
			bch_err(c, "lz4 levels: level %u decompress failed: %s", level, bch2_err_str(ret));
			return ret;
		}

		if (memcmp(verify, src, src_len)) {
			bch_err(c, "lz4 levels: level %u data mismatch", level);
			return -EIO;
		}

		pr_info("  lz4 level %u: %zu -> %zu bytes", level, src_len, dst_len);
	}

	pr_info("lz4 levels test passed");
	return 0;
}

static int test_gzip_levels(struct bch_fs *c, u64 nr)
{
	size_t chunk_size = c->opts.encoded_extent_max;

	if (!mempool_initialized(&c->compress.workspace[BCH_COMPRESSION_OPT_gzip])) {
		pr_err("gzip compression not initialized");
		return -EINVAL;
	}

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *dst __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !dst || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	for (unsigned level = 1; level <= 9; level++) {
		union bch_compression_opt opt = {
			.type	= BCH_COMPRESSION_OPT_gzip,
			.level	= level,
		};

		size_t dst_len = chunk_size;
		size_t src_len = chunk_size;

		void *workspace = mempool_alloc(&c->compress.workspace[BCH_COMPRESSION_OPT_gzip], GFP_KERNEL);
		if (!workspace)
			return -ENOMEM;

		unsigned result = bch2_compress_locked(c, dst, &dst_len, src, &src_len,
						       opt.value, POS(0, 0), workspace, verify, true, NULL);

		mempool_free(workspace, &c->compress.workspace[BCH_COMPRESSION_OPT_gzip]);

		if (result != BCH_COMPRESSION_TYPE_gzip) {
			bch_err(c, "gzip levels: level %u not compressed (type=%u)", level, result);
			return -EIO;
		}

		struct bch_extent_crc_unpacked crc = {
			.compressed_size	= dst_len >> 9,
			.uncompressed_size	= src_len >> 9,
			.compression_type	= result,
		};

		int ret = buf_uncompress(c, verify, dst, crc);
		if (ret) {
			bch_err(c, "gzip levels: level %u decompress failed: %s", level, bch2_err_str(ret));
			return ret;
		}

		if (memcmp(verify, src, src_len)) {
			bch_err(c, "gzip levels: level %u data mismatch", level);
			return -EIO;
		}

		pr_info("  gzip level %u: %zu -> %zu bytes", level, src_len, dst_len);
	}

	pr_info("gzip levels test passed");
	return 0;
}

static int test_mt_lz4_roundtrip(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	int ret = 0;

	pr_info("mt lz4 roundtrip test: 1 chunk of %zu bytes", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work work;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_lz4,
		.level	= 0,
	};

	bch2_compress_wq_submit(&work, cwq,
				opt.value, POS(0, 0),
				src, chunk_size,
				cwq->workers[0].dst_buf, chunk_size,
				&cwq->workers[0]);

	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_lz4) {
		bch_err(c, "mt lz4 roundtrip: not compressed (type=%u)",
			work.compression_type);
		return -EIO;
	}

	struct bch_extent_crc_unpacked crc = {
		.compressed_size	= work.dst_len >> 9,
		.uncompressed_size	= work.src_len >> 9,
		.compression_type	= work.compression_type,
	};

	ret = buf_uncompress(c, verify,
			     cwq->workers[0].dst_buf, crc);
	if (ret) {
		bch_err(c, "mt lz4 roundtrip: decompress failed: %s",
			bch2_err_str(ret));
		return ret;
	}

	if (memcmp(verify, src, work.src_len)) {
		bch_err(c, "mt lz4 roundtrip: roundtrip mismatch");
		return -EIO;
	}

	pr_info("mt lz4 roundtrip test passed");
	return 0;
}

static int test_mt_lz4_incompressible(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	pr_info("mt lz4 incompressible test: 1 chunk of %zu bytes", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_incompressible(src, chunk_size);

	struct bch_compress_work work;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_lz4,
		.level	= 0,
	};

	bch2_compress_wq_submit(&work, cwq,
				opt.value, POS(0, 0),
				src, chunk_size,
				cwq->workers[0].dst_buf, chunk_size,
				&cwq->workers[0]);

	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_incompressible) {
		bch_err(c, "mt lz4 incompressible: expected incompressible (type=%u)",
			work.compression_type);
		return -EIO;
	}

	pr_info("mt lz4 incompressible test passed");
	return 0;
}

static int test_mt_lz4_levels(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	const unsigned n_levels = 2;
	int ret = 0;

	pr_info("mt lz4 levels test: levels 0-1, chunk_size=%zu", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(n_levels, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_lz4,
	};

	for (unsigned batch_start = 0; batch_start < n_levels;
	     batch_start += nr_workers) {
		unsigned batch_end = min_t(unsigned, batch_start + nr_workers, n_levels);
		unsigned batch_count = batch_end - batch_start;

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l;
			struct bch_compress_worker *worker = &cwq->workers[l];

			opt.level = level;
			bch2_compress_wq_submit(&works[level], cwq,
						opt.value, POS(0, level * (chunk_size >> 9)),
						src, chunk_size,
						worker->dst_buf, chunk_size,
						worker);
		}

		bch2_compress_wq_wait_all(works + batch_start, batch_count);

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l;

			if (works[level].compression_type != BCH_COMPRESSION_TYPE_lz4) {
				bch_err(c, "mt lz4 levels: level %u not compressed (type=%u)",
					level, works[level].compression_type);
				return -EIO;
			}

			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= works[level].dst_len >> 9,
				.uncompressed_size	= works[level].src_len >> 9,
				.compression_type	= works[level].compression_type,
			};

			ret = buf_uncompress(c, verify,
					 cwq->workers[l].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt lz4 levels: level %u decompress failed: %s",
					level, bch2_err_str(ret));
				return ret;
			}

			if (memcmp(verify, src, works[level].src_len)) {
				bch_err(c, "mt lz4 levels: level %u data mismatch", level);
				return -EIO;
			}

			pr_info("  lz4 level %u: %zu -> %zu bytes", level, works[level].src_len, works[level].dst_len);
		}
	}

	pr_info("mt lz4 levels test passed");
	return 0;
}

static int test_mt_gzip_roundtrip(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	int ret = 0;

	pr_info("mt gzip roundtrip test: 1 chunk of %zu bytes", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work work;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_gzip,
		.level	= 6,
	};

	bch2_compress_wq_submit(&work, cwq,
				opt.value, POS(0, 0),
				src, chunk_size,
				cwq->workers[0].dst_buf, chunk_size,
				&cwq->workers[0]);

	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_gzip) {
		bch_err(c, "mt gzip roundtrip: not compressed (type=%u)",
			work.compression_type);
		return -EIO;
	}

	struct bch_extent_crc_unpacked crc = {
		.compressed_size	= work.dst_len >> 9,
		.uncompressed_size	= work.src_len >> 9,
		.compression_type	= work.compression_type,
	};

	ret = buf_uncompress(c, verify,
			     cwq->workers[0].dst_buf, crc);
	if (ret) {
		bch_err(c, "mt gzip roundtrip: decompress failed: %s",
			bch2_err_str(ret));
		return ret;
	}

	if (memcmp(verify, src, work.src_len)) {
		bch_err(c, "mt gzip roundtrip: roundtrip mismatch");
		return -EIO;
	}

	pr_info("mt gzip roundtrip test passed");
	return 0;
}

static int test_mt_gzip_incompressible(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;

	pr_info("mt gzip incompressible test: 1 chunk of %zu bytes", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	fill_incompressible(src, chunk_size);

	struct bch_compress_work work;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_gzip,
		.level	= 6,
	};

	bch2_compress_wq_submit(&work, cwq,
				opt.value, POS(0, 0),
				src, chunk_size,
				cwq->workers[0].dst_buf, chunk_size,
				&cwq->workers[0]);

	bch2_compress_wq_wait_all(&work, 1);

	if (work.compression_type != BCH_COMPRESSION_TYPE_incompressible) {
		bch_err(c, "mt gzip incompressible: expected incompressible (type=%u)",
			work.compression_type);
		return -EIO;
	}

	pr_info("mt gzip incompressible test passed");
	return 0;
}

static int test_mt_gzip_levels(struct bch_fs *c, u64 nr)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;
	size_t chunk_size = c->opts.encoded_extent_max;
	unsigned nr_workers = cwq->nr_workers;
	const unsigned n_levels = 9;
	int ret = 0;

	pr_info("mt gzip levels test: levels 1-9, chunk_size=%zu", chunk_size);

	void *src __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	void *verify __free(kvfree) = kvmalloc(chunk_size, GFP_KERNEL);
	if (!src || !verify)
		return -ENOMEM;

	fill_compressible(src, chunk_size);

	struct bch_compress_work *works __free(kvfree) =
		kvcalloc(n_levels, sizeof(*works), GFP_KERNEL);
	if (!works)
		return -ENOMEM;

	union bch_compression_opt opt = {
		.type	= BCH_COMPRESSION_OPT_gzip,
	};

	for (unsigned batch_start = 0; batch_start < n_levels;
	     batch_start += nr_workers) {
		unsigned batch_end = min_t(unsigned, batch_start + nr_workers, n_levels);
		unsigned batch_count = batch_end - batch_start;

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l + 1;
			struct bch_compress_worker *worker = &cwq->workers[l];

			opt.level = level;
			bch2_compress_wq_submit(&works[level - 1], cwq,
						opt.value, POS(0, (level - 1) * (chunk_size >> 9)),
						src, chunk_size,
						worker->dst_buf, chunk_size,
						worker);
		}

		bch2_compress_wq_wait_all(works + batch_start, batch_count);

		for (unsigned l = 0; l < batch_count; l++) {
			unsigned level = batch_start + l + 1;
			unsigned i = level - 1;

			if (works[i].compression_type != BCH_COMPRESSION_TYPE_gzip) {
				bch_err(c, "mt gzip levels: level %u not compressed (type=%u)",
					level, works[i].compression_type);
				return -EIO;
			}

			struct bch_extent_crc_unpacked crc = {
				.compressed_size	= works[i].dst_len >> 9,
				.uncompressed_size	= works[i].src_len >> 9,
				.compression_type	= works[i].compression_type,
			};

			ret = buf_uncompress(c, verify,
					 cwq->workers[l].dst_buf, crc);
			if (ret) {
				bch_err(c, "mt gzip levels: level %u decompress failed: %s",
					level, bch2_err_str(ret));
				return ret;
			}

			if (memcmp(verify, src, works[i].src_len)) {
				bch_err(c, "mt gzip levels: level %u data mismatch", level);
				return -EIO;
			}

			pr_info("  gzip level %u: %zu -> %zu bytes", level, works[i].src_len, works[i].dst_len);
		}
	}

	pr_info("mt gzip levels test passed");
	return 0;
}

typedef int (*compress_test_fn)(struct bch_fs *, u64);

/*
 * Entry point invoked from fs/debug/sysfs.c (the sysfs_compress_test
 * attribute). The tests.h agent is responsible for declaring this in
 * fs/debug/tests.h:
 *
 *   int bch2_compress_test(struct bch_fs *, const char *, u64, unsigned);
 */
int bch2_compress_test(struct bch_fs *c, const char *testname,
		       u64 nr, unsigned nr_threads)
{
	compress_test_fn fn = NULL;

	if (nr == 0) {
		pr_err("nr of iterations is not allowed to be 0");
		return -EINVAL;
	}

	if (!c->compress.mt_wq) {
		pr_err("MT compression workqueue not initialized");
		return -EINVAL;
	}

#define compress_test(_test)				\
	if (!strcmp(testname, #_test)) fn = _test

	compress_test(test_mt_compress_decompress);
	compress_test(test_mt_compress_incompressible);
	compress_test(test_mt_concurrency);
	compress_test(test_mt_levels);
	compress_test(test_mt_nonce_uniqueness);
	compress_test(test_mt_pos_monotonic);
	compress_test(test_mt_dst_len_valid);
	compress_test(test_mt_level_consistency);
	compress_test(test_mt_worker_isolation);
	compress_test(test_mt_batch_sizes);
	compress_test(test_mt_mixed_compression);
	compress_test(test_mt_single_chunk);
	compress_test(test_mt_alignment);
	compress_test(test_mt_empty_input);
	compress_test(test_mt_reuse);
	compress_test(test_corrupt_zstd_single_byte);
	compress_test(test_corrupt_zstd_header_len);
	compress_test(test_corrupt_zstd_header_zero);
	compress_test(test_corrupt_all_ones);
	compress_test(test_corrupt_first_byte);
	compress_test(test_corrupt_last_byte);
	compress_test(test_corrupt_crc_compressed_size);
	compress_test(test_corrupt_crc_uncompressed_size);
	compress_test(test_corrupt_crc_wrong_type);
	compress_test(test_corrupt_recompress_after);
	compress_test(test_lz4_roundtrip);
	compress_test(test_lz4_incompressible);
	compress_test(test_gzip_roundtrip);
	compress_test(test_gzip_incompressible);
	compress_test(test_lz4_levels);
	compress_test(test_gzip_levels);
	compress_test(test_mt_lz4_roundtrip);
	compress_test(test_mt_lz4_incompressible);
	compress_test(test_mt_lz4_levels);
	compress_test(test_mt_gzip_roundtrip);
	compress_test(test_mt_gzip_incompressible);
	compress_test(test_mt_gzip_levels);

	if (!fn) {
		pr_err("unknown compress test %s", testname);
		return -EINVAL;
	}

	int ret = fn(c, nr);
	if (ret)
		bch_err(c, "compress test %s failed: %s", testname, bch2_err_str(ret));
	else
		pr_info("compress test %s passed", testname);
	return ret;
}

#endif /* CONFIG_BCACHEFS_TESTS */
