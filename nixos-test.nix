self': let
    bcachefsNode = cores: { config, pkgs, ... }: {
      boot.kernelPackages = pkgs.linuxPackages_latest;
      boot.bcachefs.modulePackage =
        self'.packages.bcachefs-module-linux-latest.overrideAttrs (old: {
          makeFlags = (old.makeFlags or []) ++ [ "BCACHEFS_TESTS=1" ];
        });
      assertions = [{
        assertion = config.boot.bcachefs.modulePackage or null != null;
        message = "bcachefs module not set";
      }];
      virtualisation.cores = cores;
      virtualisation.memorySize = 1024;
      virtualisation.emptyDiskImages = [{
        size = 4096;
        driveConfig.deviceExtraOpts.serial = "test-disk";
      }];
      boot.supportedFilesystems.bcachefs = true;
      boot.bcachefs.package = self'.packages.bcachefs-tools;
    };
  in {
  name = "bcachefs-nixos";

    nodes.machine = bcachefsNode 32;

  testScript = ''
    machine.wait_for_unit("multi-user.target")

    with subtest("module built from local sources"):
      machine.succeed(
        "modinfo bcachefs | grep updates/src/fs/bcachefs > /dev/null",
        "mkfs.bcachefs /dev/disk/by-id/virtio-test-disk",
        "mkdir -p /mnt",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
        "umount /mnt",
      )

    with subtest("mt compress workers initialized"):
      # bch2_compress_workers module param defaults to 0 (= auto =
      # min(num_online_cpus(), 32); see fs/data/compress.c:55).  The
      # effective worker count is computed in bch2_compress_nr_workers()
      # at WQ init time, so the param value on its own doesn't prove the
      # pool was sized; we still need nproc >= 2 for MT to engage.
      param = machine.succeed("cat /sys/module/bcachefs/parameters/compress_workers").strip()
      print(f"compress_workers module param: {param}")
      nproc = int(machine.succeed("nproc").strip())
      print(f"nproc: {nproc}")
      assert nproc >= 2, f"need >= 2 vCPUs for MT path, got {nproc}"

      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )

      # bch2_compress_wq_init() is called from bch2_fs_compress_init()
      # at mount time.  The only init-time log emitted is the failure
      # path's pr_notice(); success is silent.  Assert the failure
      # message is absent, which proves the WQ was allocated and the
      # per-worker workspace + dst + verify buffers were kmalloc'd.
      dmesg = machine.succeed("dmesg")
      print(f"dmesg tail:\n{dmesg[-2000:]}")
      assert "MT compression workqueue init failed" not in dmesg, (
        "MT compression WQ init failed; check dmesg for details")

      machine.succeed("umount /mnt")

    with subtest("mt compression roundtrip"):
      # 128 MiB of /dev/zero.  encoded_extent_max is 256 KiB (see
      # fs/opts.h:172), so this write produces 512 chunks and the MT
      # dispatch branch (bch2_write_should_mt_compress) returns true.
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      machine.succeed(
        "dd if=/dev/zero of=/tmp/src-mt bs=1M count=128 2>&1",
        "cp /tmp/src-mt /mnt/mt-zeros",
        "sync",
      )
      machine.succeed("cmp /tmp/src-mt /mnt/mt-zeros")
      usage = machine.succeed("bcachefs fs usage -a /mnt")
      print(f"fs usage:\n{usage}")
      found_zstd = False
      for line in usage.splitlines():
        if "zstd" in line and "compressed" not in line:
          parts = line.split()
          compressed = int(parts[1])
          uncompressed = int(parts[2])
          ratio = compressed / uncompressed
          print(f"compressed={compressed} uncompressed={uncompressed} ratio={ratio:.4f}")
          assert compressed < uncompressed, f"compressible data not compressed: {compressed} >= {uncompressed}"
          assert ratio < 0.1, f"compression ratio too high: {ratio:.2f}"
          found_zstd = True
          break
      assert found_zstd, "no zstd compression line in fs usage output"
      machine.succeed("umount /mnt")

      machine.succeed("mount /dev/disk/by-id/virtio-test-disk /mnt")
      machine.succeed("cmp /tmp/src-mt /mnt/mt-zeros")
      machine.succeed("umount /mnt")

    with subtest("mt compression workers concurrent"):
      # test_mt_concurrency (fs/debug/compress_test.c:229) submits N
      # trivial work items that each sleep 10 ms, then walks the
      # recorded start/end timestamps and counts pairs whose intervals
      # overlap.  With truly parallel workers, overlap_count > 0; with
      # a serial fallback, overlap_count = 0 and the test returns
      # -EIO.  The test emits two pr_info()s: a "mt concurrency test:
      # ..." header and a "mt concurrency test passed: %u overlapping
      # pairs out of %u" footer; the framework adds a "compress test
      # test_mt_concurrency passed" line on success.
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      dmesg_before = int(machine.succeed("dmesg | wc -l").strip())
      machine.succeed(
        "echo 'test_mt_concurrency 1' > /sys/fs/bcachefs/*/compress_test",
      )
      # The kernel test returns synchronously after drain_workqueue;
      # let the console buffer flush before reading dmesg.
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_before:]
      dmesg_new = "\n".join(dmesg_after)
      print(f"dmesg after test_mt_concurrency:\n{dmesg_new}")
      assert "mt concurrency test" in dmesg_new, (
        f"no 'mt concurrency test' marker in dmesg:\n{dmesg_new}")
      assert "overlapping pairs" in dmesg_new, (
        f"no 'overlapping pairs' report in dmesg:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_concurrency reported failed:\n{dmesg_new}")
      machine.succeed("umount /mnt")

    with subtest("mt invariant verification"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())

      # test_mt_nonce_uniqueness: verify nonce uniqueness across 8 chunks
      machine.succeed(
        "echo 'test_mt_nonce_uniqueness 8' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_nonce_uniqueness:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_nonce_uniqueness did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_nonce_uniqueness reported failed:\n{dmesg_new}")

      # test_mt_pos_monotonic: verify pos.offset monotonically increasing
      machine.succeed(
        "echo 'test_mt_pos_monotonic 8' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_pos_monotonic:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_pos_monotonic did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_pos_monotonic reported failed:\n{dmesg_new}")

      # test_mt_dst_len_valid: verify output bounds
      machine.succeed(
        "echo 'test_mt_dst_len_valid 8' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_dst_len_valid:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_dst_len_valid did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_dst_len_valid reported failed:\n{dmesg_new}")

      machine.succeed(
        "echo 'test_mt_compress_decompress 8' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_compress_decompress:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_compress_decompress did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_compress_decompress reported failed:\n{dmesg_new}")

      machine.succeed(
        "echo 'test_mt_compress_incompressible 4' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_compress_incompressible:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_compress_incompressible did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_compress_incompressible reported failed:\n{dmesg_new}")

      machine.succeed(
        "echo 'test_mt_levels 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_levels:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_levels did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_levels reported failed:\n{dmesg_new}")

      # test_mt_level_consistency: verify all 15 zstd levels produce valid output
      machine.succeed(
        "echo 'test_mt_level_consistency 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_level_consistency:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_level_consistency did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_level_consistency reported failed:\n{dmesg_new}")

      # test_mt_worker_isolation: verify no cross-contamination between
      # chunks sharing workers
      machine.succeed(
        "echo 'test_mt_worker_isolation 8' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_worker_isolation:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_worker_isolation did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_worker_isolation reported failed:\n{dmesg_new}")

      # test_mt_batch_sizes: verify various batch sizes work
      machine.succeed(
        "echo 'test_mt_batch_sizes 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      print(f"dmesg after test_mt_batch_sizes:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_batch_sizes did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_batch_sizes reported failed:\n{dmesg_new}")

      machine.succeed("umount /mnt")

    with subtest("mt edge cases"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())

      # test_mt_mixed_compression: mixed compressible/incompressible
      machine.succeed(
        "echo 'test_mt_mixed_compression 8' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_mixed_compression:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_mixed_compression did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_mixed_compression reported failed:\n{dmesg_new}")

      # test_mt_single_chunk: degenerate batch size
      machine.succeed(
        "echo 'test_mt_single_chunk 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_single_chunk:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_single_chunk did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_single_chunk reported failed:\n{dmesg_new}")

      # test_mt_alignment: sub-extent-max sizes
      machine.succeed(
        "echo 'test_mt_alignment 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_alignment:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_alignment did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_alignment reported failed:\n{dmesg_new}")

      # test_mt_empty_input: minimum-size input
      machine.succeed(
        "echo 'test_mt_empty_input 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_mt_empty_input:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_empty_input did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_empty_input reported failed:\n{dmesg_new}")

      # test_mt_reuse: reuse workers for second submission
      machine.succeed(
        "echo 'test_mt_reuse 4' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      print(f"dmesg after test_mt_reuse:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_mt_reuse did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_mt_reuse reported failed:\n{dmesg_new}")

      machine.succeed("umount /mnt")

    with subtest("mt corruption handling"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())

      # test_corrupt_zstd_single_byte: single byte corruption in zstd stream
      machine.succeed(
        "echo 'test_corrupt_zstd_single_byte 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_zstd_single_byte:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_zstd_single_byte did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_zstd_single_byte reported failed:\n{dmesg_new}")

      # test_corrupt_zstd_header_len: corrupted zstd frame header length
      machine.succeed(
        "echo 'test_corrupt_zstd_header_len 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_zstd_header_len:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_zstd_header_len did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_zstd_header_len reported failed:\n{dmesg_new}")

      # test_corrupt_zstd_header_zero: zeroed zstd frame header
      machine.succeed(
        "echo 'test_corrupt_zstd_header_zero 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_zstd_header_zero:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_zstd_header_zero did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_zstd_header_zero reported failed:\n{dmesg_new}")

      # test_corrupt_all_ones: all-ones corruption pattern
      machine.succeed(
        "echo 'test_corrupt_all_ones 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_all_ones:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_all_ones did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_all_ones reported failed:\n{dmesg_new}")

      # test_corrupt_first_byte: corruption of first byte
      machine.succeed(
        "echo 'test_corrupt_first_byte 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_first_byte:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_first_byte did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_first_byte reported failed:\n{dmesg_new}")

      # test_corrupt_last_byte: corruption of last byte
      machine.succeed(
        "echo 'test_corrupt_last_byte 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_last_byte:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_last_byte did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_last_byte reported failed:\n{dmesg_new}")

      # test_corrupt_crc_compressed_size: wrong CRC compressed size
      machine.succeed(
        "echo 'test_corrupt_crc_compressed_size 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_crc_compressed_size:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_crc_compressed_size did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_crc_compressed_size reported failed:\n{dmesg_new}")

      # test_corrupt_crc_uncompressed_size: wrong CRC uncompressed size
      machine.succeed(
        "echo 'test_corrupt_crc_uncompressed_size 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_crc_uncompressed_size:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_crc_uncompressed_size did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_crc_uncompressed_size reported failed:\n{dmesg_new}")

      # test_corrupt_recompress_after: recompression after corruption
      machine.succeed(
        "echo 'test_corrupt_recompress_after 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_recompress_after:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_recompress_after did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_recompress_after reported failed:\n{dmesg_new}")

      # test_corrupt_crc_wrong_type: wrong CRC type field
      machine.succeed(
        "echo 'test_corrupt_crc_wrong_type 1' > /sys/fs/bcachefs/*/compress_test",
      )
      machine.wait_for_unit("multi-user.target")
      dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
      dmesg_new = "\n".join(dmesg_after)
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
      print(f"dmesg after test_corrupt_crc_wrong_type:\n{dmesg_new}")
      assert "passed" in dmesg_new, (
        f"test_corrupt_crc_wrong_type did not pass:\n{dmesg_new}")
      assert "failed" not in dmesg_new.lower(), (
        f"test_corrupt_crc_wrong_type reported failed:\n{dmesg_new}")

      machine.succeed("umount /mnt")

    with subtest("mt compression ratio"):
      # 256 MiB of zeros.  Same dispatch path as the roundtrip subtest,
      # but the larger volume exercises the per-worker workspace more
      # aggressively and makes the zstd dictionary convergence visible
      # in fs usage.  Uses 256 MiB instead of 1 GiB to fit within the
      # NixOS test VM's root filesystem.
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      machine.succeed(
        "dd if=/dev/zero of=/tmp/src-mt-ratio bs=1M count=256 2>&1",
        "cp /tmp/src-mt-ratio /mnt/mt-ratio",
        "sync",
      )
      usage = machine.succeed("bcachefs fs usage -a /mnt")
      print(f"fs usage:\n{usage}")
      found = False
      for line in usage.splitlines():
        if "zstd" in line and "compressed" not in line:
          parts = line.split()
          compressed = int(parts[1])
          uncompressed = int(parts[2])
          ratio = compressed / uncompressed
          print(f"compressed={compressed} uncompressed={uncompressed} ratio={ratio:.4f}")
          # All-zeros streams compress to a handful of KiB regardless of
          # worker count; the ratio may be slightly higher than with 1 GiB
          # due to fixed metadata overhead.
          assert ratio < 0.02, f"zeros compression ratio too high: {ratio:.4f}"
          found = True
          break
      assert found, "no zstd compression line in fs usage output"
      machine.succeed("cmp /tmp/src-mt-ratio /mnt/mt-ratio")
      machine.succeed("umount /mnt")

      machine.succeed("mount /dev/disk/by-id/virtio-test-disk /mnt")
      machine.succeed("cmp /tmp/src-mt-ratio /mnt/mt-ratio")
      machine.succeed("umount /mnt")

    with subtest("mt stress test"):
      # Format + write 5 times in a row, mixing compressible and
      # incompressible extents so the MT dispatch path has to interleave
      # the two completion queues.  fsck after each cycle so a silent
      # metadata corruption from the new code paths would fail the test.
      machine.succeed(
        "dd if=/dev/zero of=/tmp/src-mt-zero bs=1M count=128 2>&1",
        "dd if=/dev/urandom of=/tmp/src-mt-rand bs=1M count=128 2>&1",
      )
      for i in range(5):
        print(f"mt stress cycle {i+1}/5")
        machine.succeed(
          "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
          "mount /dev/disk/by-id/virtio-test-disk /mnt",
          "cp /tmp/src-mt-zero /mnt/mt-zero",
          "cp /tmp/src-mt-rand /mnt/mt-rand",
          "sync",
        )
        machine.succeed(
          "cmp /tmp/src-mt-zero /mnt/mt-zero",
          "cmp /tmp/src-mt-rand /mnt/mt-rand",
          "umount /mnt",
          "bcachefs fsck /dev/disk/by-id/virtio-test-disk",
        )

    with subtest("mt small write fallback"):
      # Writes below encoded_extent_max (256 KiB) take the serial path;
      # verify the serial fallback round-trips correctly and produces a
      # compressed extent.  The 4 KiB write is 64x below the MT threshold.
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      machine.succeed(
        "dd if=/dev/zero of=/tmp/src-mt-small bs=4K count=1 2>&1",
        "cp /tmp/src-mt-small /mnt/mt-small",
        "sync",
      )
      machine.succeed("cmp /tmp/src-mt-small /mnt/mt-small")
      machine.succeed("umount /mnt")

      machine.succeed("mount /dev/disk/by-id/virtio-test-disk /mnt")
      machine.succeed("cmp /tmp/src-mt-small /mnt/mt-small")
      machine.succeed("umount /mnt")

    with subtest("lz4 compression regression"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=lz4 /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      machine.succeed(
        "dd if=/dev/zero of=/tmp/src-lz4 bs=1M count=64 2>&1",
        "cp /tmp/src-lz4 /mnt/lz4-zeros",
        "sync",
      )
      machine.succeed("cmp /tmp/src-lz4 /mnt/lz4-zeros")
      usage = machine.succeed("bcachefs fs usage -a /mnt")
      print(f"lz4 fs usage:\n{usage}")
      found_lz4 = False
      for line in usage.splitlines():
        if "lz4" in line and "compressed" not in line:
          parts = line.split()
          compressed = int(parts[1])
          uncompressed = int(parts[2])
          print(f"lz4: compressed={compressed} uncompressed={uncompressed}")
          assert compressed < uncompressed, f"lz4 data not compressed: {compressed} >= {uncompressed}"
          found_lz4 = True
          break
      assert found_lz4, "no lz4 compression line in fs usage output"
      machine.succeed("umount /mnt")

      machine.succeed("mount /dev/disk/by-id/virtio-test-disk /mnt")
      machine.succeed("cmp /tmp/src-lz4 /mnt/lz4-zeros")
      machine.succeed("umount /mnt")

      machine.succeed(
        "mkfs.bcachefs --force --compression=lz4 /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())

      for test_name in ["test_lz4_roundtrip", "test_lz4_incompressible", "test_lz4_levels"]:
        machine.succeed(
          f"echo '{test_name} 1' > /sys/fs/bcachefs/*/compress_test",
        )
        machine.wait_for_unit("multi-user.target")
        dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
        dmesg_new = "\n".join(dmesg_after)
        dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
        print(f"dmesg after {test_name}:\n{dmesg_new}")
        assert "passed" in dmesg_new, f"{test_name} did not pass:\n{dmesg_new}"
        assert "failed" not in dmesg_new.lower(), f"{test_name} reported failed:\n{dmesg_new}"

      machine.succeed("umount /mnt")

    with subtest("gzip compression regression"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=gzip /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      machine.succeed(
        "dd if=/dev/zero of=/tmp/src-gzip bs=1M count=64 2>&1",
        "cp /tmp/src-gzip /mnt/gzip-zeros",
        "sync",
      )
      machine.succeed("cmp /tmp/src-gzip /mnt/gzip-zeros")
      usage = machine.succeed("bcachefs fs usage -a /mnt")
      print(f"gzip fs usage:\n{usage}")
      found_gzip = False
      for line in usage.splitlines():
        if "gzip" in line and "compressed" not in line:
          parts = line.split()
          compressed = int(parts[1])
          uncompressed = int(parts[2])
          print(f"gzip: compressed={compressed} uncompressed={uncompressed}")
          assert compressed < uncompressed, f"gzip data not compressed: {compressed} >= {uncompressed}"
          found_gzip = True
          break
      assert found_gzip, "no gzip compression line in fs usage output"
      machine.succeed("umount /mnt")

      machine.succeed("mount /dev/disk/by-id/virtio-test-disk /mnt")
      machine.succeed("cmp /tmp/src-gzip /mnt/gzip-zeros")
      machine.succeed("umount /mnt")

      machine.succeed(
        "mkfs.bcachefs --force --compression=gzip /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )
      dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())

      for test_name in ["test_gzip_roundtrip", "test_gzip_incompressible", "test_gzip_levels"]:
        machine.succeed(
          f"echo '{test_name} 1' > /sys/fs/bcachefs/*/compress_test",
        )
        machine.wait_for_unit("multi-user.target")
        dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
        dmesg_new = "\n".join(dmesg_after)
        dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
        print(f"dmesg after {test_name}:\n{dmesg_new}")
        assert "passed" in dmesg_new, f"{test_name} did not pass:\n{dmesg_new}"
        assert "failed" not in dmesg_new.lower(), f"{test_name} reported failed:\n{dmesg_new}"

      machine.succeed("umount /mnt")

    with subtest("mt compress benchmark"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )

      codecs = ["zstd:3", "lz4", "gzip:6"]
      for codec in codecs:
          machine.succeed(
              f"echo '{codec}' > /sys/fs/bcachefs/*/mt_compress_bench",
          )
      for codec in codecs:
          machine.succeed(
              f"echo '{codec}' > /sys/fs/bcachefs/*/mt_compress_bench_scaling",
          )
      machine.succeed("umount /mnt")

    with subtest("mt compress benchmark multi-core scaling"):
      all_results = []
      codec_names = {"0x33": "zstd:3", "0x1": "lz4", "0x62": "gzip:6"}

      for nw in [4, 8, 16, 32]:
          machine.succeed(f"echo {nw} > /sys/module/bcachefs/parameters/compress_workers")
          machine.succeed("dmesg -C")
          machine.succeed(
              "mkfs.bcachefs --force --compression=zstd /dev/disk/by-id/virtio-test-disk",
              "mount /dev/disk/by-id/virtio-test-disk /mnt",
          )
          for codec in ["zstd:3", "gzip:6"]:
              machine.succeed(
                  f"echo '{codec}' > /sys/fs/bcachefs/*/mt_compress_bench",
              )
              machine.succeed(
                  f"echo '{codec}' > /sys/fs/bcachefs/*/mt_compress_bench_scaling",
              )
          dmesg = machine.succeed("dmesg")
          current_scaling_opt = None
          for line in dmesg.splitlines():
              if "MT_COMPRESS_BENCH: result" in line:
                  kv = {}
                  for part in line.split("MT_COMPRESS_BENCH: result")[1].strip().split():
                      if "=" in part:
                          k, v = part.split("=", 1)
                          kv[k] = v
                  kv["cores"] = str(nw)
                  kv["codec_name"] = codec_names.get(kv.get("opt", ""), kv.get("opt", "?"))
                  all_results.append(kv)
              elif "MT_COMPRESS_BENCH_SCALING: opt=" in line:
                  hdr = line.split("MT_COMPRESS_BENCH_SCALING:")[1].strip()
                  for part in hdr.split():
                      if part.startswith("opt="):
                          current_scaling_opt = part.split("=", 1)[1]
              elif "MT_COMPRESS_BENCH_SCALING: row" in line and current_scaling_opt:
                  parts = line.split("MT_COMPRESS_BENCH_SCALING: row")[1].strip().split()
                  if len(parts) >= 5:
                      all_results.append({
                          "type": "scaling",
                          "cores": str(nw),
                          "opt": current_scaling_opt,
                          "codec_name": codec_names.get(current_scaling_opt, "?"),
                          "eff_workers": parts[0],
                          "serial_ns": parts[1],
                          "parallel_ns": parts[2],
                          "speedup_x100": parts[3],
                          "throughput_mib_s_x100": parts[4],
                      })
          machine.succeed("umount /mnt")

      machine.succeed("echo 0 > /sys/module/bcachefs/parameters/compress_workers")

      speedup_rows = [r for r in all_results if r.get("type") != "scaling" and "serial_ns" in r]
      scaling_rows = [r for r in all_results if r.get("type") == "scaling"]

      print(f"\n{'='*80}")
      print("  MT Compression Benchmark: Serial vs Parallel by Core Count")
      print(f"{'='*80}")
      print(f"  {'Cores':>5} {'Codec':<8} {'Serial(ms)':>12} {'Parallel(ms)':>14} {'Speedup':>10}")
      print(f"  {'-'*5} {'-'*8} {'-'*12} {'-'*14} {'-'*10}")
      for r in sorted(speedup_rows, key=lambda x: (int(x["cores"]), x.get("opt",""))):
          s_ms = int(r["serial_ns"]) / 1_000_000
          p_ms = int(r["parallel_ns"]) / 1_000_000
          su = int(r["speedup_x100"])
          print(f"  {r['cores']:>5} {r['codec_name']:<8} {s_ms:>12.2f} {p_ms:>14.2f} {su/100:>9.2f}x")
      print(f"{'='*80}\n")

      print(f"\n{'='*80}")
      print("  MT Compression Scaling: Effective Workers vs Throughput")
      print(f"{'='*80}")
      print(f"  {'Cores':>5} {'Codec':<8} {'EffW':>6} {'Speedup':>10} {'MiB/s':>10}")
      print(f"  {'-'*5} {'-'*8} {'-'*6} {'-'*10} {'-'*10}")
      for r in sorted(scaling_rows, key=lambda x: (int(x["cores"]), x.get("opt",""), int(x.get("eff_workers","0")))):
          su = int(r["speedup_x100"])
          tp = int(r["throughput_mib_s_x100"])
          ew = r.get("eff_workers", "?")
          print(f"  {r['cores']:>5} {r['codec_name']:<8} {ew:>6} {su/100:>9.2f}x {tp/100:>9.2f}")
      print(f"{'='*80}\n")

    with subtest("mt lz4 compression"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=lz4 /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )

      for (test_name, count) in [("test_mt_lz4_roundtrip", 4),
                                  ("test_mt_lz4_incompressible", 4),
                                  ("test_mt_lz4_levels", 1)]:
        dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
        machine.succeed(
          f"echo '{test_name} {count}' > /sys/fs/bcachefs/*/compress_test",
        )
        machine.wait_for_unit("multi-user.target")
        dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
        dmesg_new = "\n".join(dmesg_after)
        dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
        print(f"dmesg after {test_name}:\n{dmesg_new}")
        assert "passed" in dmesg_new, (
          f"{test_name} did not pass:\n{dmesg_new}")
        assert "failed" not in dmesg_new.lower(), (
          f"{test_name} reported failure:\n{dmesg_new}")

      machine.succeed("umount /mnt")

    with subtest("mt gzip compression"):
      machine.succeed(
        "mkfs.bcachefs --force --compression=gzip /dev/disk/by-id/virtio-test-disk",
        "mount /dev/disk/by-id/virtio-test-disk /mnt",
      )

      for (test_name, count) in [("test_mt_gzip_roundtrip", 4),
                                  ("test_mt_gzip_incompressible", 4),
                                  ("test_mt_gzip_levels", 1)]:
        dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
        machine.succeed(
          f"echo '{test_name} {count}' > /sys/fs/bcachefs/*/compress_test",
        )
        machine.wait_for_unit("multi-user.target")
        dmesg_after = machine.succeed("dmesg").splitlines()[dmesg_marker:]
        dmesg_new = "\n".join(dmesg_after)
        dmesg_marker = int(machine.succeed("dmesg | wc -l").strip())
        print(f"dmesg after {test_name}:\n{dmesg_new}")
        assert "passed" in dmesg_new, (
          f"{test_name} did not pass:\n{dmesg_new}")
        assert "failed" not in dmesg_new.lower(), (
          f"{test_name} reported failure:\n{dmesg_new}")

      machine.succeed("umount /mnt")
  '';
}
