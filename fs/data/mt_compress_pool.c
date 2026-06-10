// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"

#include "data/compress.h"
#include "data/mt_compress_pool.h"
#include "util/util.h"

#include <linux/lz4.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/zlib.h>
#include <linux/zstd.h>

#ifndef __KERNEL__
#include <pthread.h>
#include <sched.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <string.h>
#define kvmalloc(size, flags) malloc(size)
#define kvfree(ptr) free(ptr)
#define cpu_to_node(cpu) 0
#define kmalloc_node(size, flags, node) ((void)(node), kmalloc(size, flags))
#else
#include <linux/kthread.h>
#include <linux/wait.h>
#endif

static inline void *pool_alloc(size_t size, unsigned align)
{
#ifndef __KERNEL__
	(void)align;
	return aligned_alloc(align, size);
#else
	return kmalloc(size, GFP_KERNEL);
#endif
}

static inline void pool_free(void *ptr)
{
#ifndef __KERNEL__
	free(ptr);
#else
	kfree(ptr);
#endif
}

static inline void *pool_zalloc(size_t size, unsigned align)
{
#ifndef __KERNEL__
	(void)align;
	void *p = aligned_alloc(align, size);
	if (p) memset(p, 0, size);
	return p;
#else
	return kzalloc(size, GFP_KERNEL);
#endif
}

static inline void completion_wake(struct mt_completion *completion)
{
#ifndef __KERNEL__
	atomic_set(&completion->futex_word, 1);
	syscall(SYS_futex, &completion->futex_word.counter,
		FUTEX_WAKE, 1, NULL, NULL, 0);
#else
	wake_up(&completion->wait);
#endif
}

static inline void completion_wait(struct mt_completion *completion)
{
#ifndef __KERNEL__
	while (!atomic_read_acquire(&completion->futex_word))
		syscall(SYS_futex, &completion->futex_word.counter,
			FUTEX_WAIT, 0, NULL, NULL, 0);
#else
	wait_event(completion->wait,
		   !atomic_read(&completion->count));
#endif
}

#ifndef __KERNEL__
static void *mt_worker_fn(void *arg)
#else
static int mt_worker_fn(void *arg)
#endif
{
	struct mt_worker_cold *cold = arg;
	struct mt_worker_slot *slot = cold->slot;
	struct bch_fs *c = cold->c;
	u64 expected_gen = 0;

	for (;;) {
		u64 s;

		while (!((s = atomic64_read_acquire(&slot->state)) & 1) ||
		       (s >> 1) != expected_gen) {
			if (cold->should_stop) {
				s = atomic64_read_acquire(&slot->state);
				if ((s & 1) && (s >> 1) == expected_gen)
					goto do_work;
#ifndef __KERNEL__
				return NULL;
#else
				return 0;
#endif
			}
			cpu_relax();
		}
do_work:
		{
			size_t dst_len = slot->extent_max;
			size_t src_len = slot->src_len;

			slot->compression_type = bch2_compress_locked(
				c,
				slot->dst_buf, &dst_len,
				slot->src_buf, &src_len,
				slot->compression_opt,
				slot->write_pos,
				slot->workspace,
				slot->verify_buf,
				false,
				slot->gzip_inited);

			slot->result_dst_len = dst_len;
			slot->result_src_len = src_len;
		}

		atomic64_set_release(&slot->state, (expected_gen + 1) << 1);
		expected_gen++;
		cold->expected_gen = expected_gen;

		if (atomic_dec_return_release(&slot->completion->count) == 0)
			completion_wake(slot->completion);
	}
}

static size_t mt_workspace_size(struct bch_fs *c)
{
	size_t ws_size = 0;

	if (c->opts.compression) {
		union bch_compression_opt co = { .value = c->opts.compression };
		switch (co.type) {
		case BCH_COMPRESSION_OPT_lz4:
			ws_size = max_t(size_t, LZ4_MEM_COMPRESS, LZ4HC_MEM_COMPRESS);
			break;
		case BCH_COMPRESSION_OPT_gzip:
			ws_size = zlib_deflate_workspacesize(MAX_WBITS, DEF_MEM_LEVEL);
			break;
		case BCH_COMPRESSION_OPT_zstd:
			ws_size = READ_ONCE(c->compress.zstd_workspace_size);
			break;
		}
	}
	if (c->opts.background_compression) {
		union bch_compression_opt co = { .value = c->opts.background_compression };
		switch (co.type) {
		case BCH_COMPRESSION_OPT_lz4:
			ws_size = max_t(size_t, ws_size, LZ4_MEM_COMPRESS);
			ws_size = max_t(size_t, ws_size, LZ4HC_MEM_COMPRESS);
			break;
		case BCH_COMPRESSION_OPT_gzip:
			ws_size = max_t(size_t, ws_size,
					zlib_deflate_workspacesize(MAX_WBITS, DEF_MEM_LEVEL));
			break;
		case BCH_COMPRESSION_OPT_zstd:
			ws_size = max_t(size_t, ws_size,
					READ_ONCE(c->compress.zstd_workspace_size));
			break;
		}
	}
	if (!ws_size)
		ws_size = READ_ONCE(c->compress.zstd_workspace_size);

	return ws_size;
}

static int mt_start_workers(struct mt_compress_pool *pool)
{
	for (unsigned i = 0; i < pool->nr_workers; i++) {
		struct mt_worker_cold *cold = &pool->cold[i];
#ifndef __KERNEL__
		pthread_attr_t attr;
		pthread_attr_init(&attr);

		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(i % num_online_cpus(), &cpuset);
		pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);

		int r = pthread_create(&cold->thread, &attr, mt_worker_fn, cold);
		pthread_attr_destroy(&attr);
		if (r)
			return -r;
#else
		cold->task = kthread_create(mt_worker_fn, cold, "mt_comp/%u", i);
		if (IS_ERR(cold->task))
			return PTR_ERR(cold->task);
		kthread_bind(cold->task, i % num_online_cpus());
		wake_up_process(cold->task);
#endif
	}
	return 0;
}

int mt_compress_pool_init(struct bch_fs *c)
{
	unsigned nr = min_t(unsigned, bch2_compress_nr_workers(), 4);
	size_t ws_size = mt_workspace_size(c);
	unsigned extent_max = c->opts.encoded_extent_max;
	int ret = 0;

	if (nr > MT_MAX_WORKERS)
		nr = MT_MAX_WORKERS;

	size_t per_worker = ws_size + extent_max * 3;
	size_t max_pool_mem = 256 * 1024 * 1024;
	unsigned mem_cap = max_pool_mem / per_worker;
	if (nr > mem_cap)
		nr = mem_cap;

	struct mt_compress_pool *pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return -ENOMEM;

	pool->nr_workers = nr;

	pool->slots = pool_zalloc(nr * sizeof(struct mt_worker_slot), 128);
	if (!pool->slots) {
		ret = -ENOMEM;
		goto err_pool;
	}

	pool->cold = pool_zalloc(nr * sizeof(struct mt_worker_cold), 64);
	if (!pool->cold) {
		ret = -ENOMEM;
		goto err_slots;
	}

	pool->gzip_inited_arr = kcalloc(nr, sizeof(bool), GFP_KERNEL);
	if (!pool->gzip_inited_arr) {
		ret = -ENOMEM;
		goto err_cold;
	}

	for (unsigned i = 0; i < nr; i++) {
		struct mt_worker_slot *slot = &pool->slots[i];
		struct mt_worker_cold *cold = &pool->cold[i];

		slot->workspace = kvmalloc(ws_size, GFP_KERNEL);
		if (!slot->workspace) {
			ret = -ENOMEM;
			goto err_workers;
		}

		slot->src_buf = kmalloc(extent_max, GFP_KERNEL);
		if (!slot->src_buf) {
			ret = -ENOMEM;
			goto err_workers;
		}

		slot->dst_buf = kmalloc(extent_max, GFP_KERNEL);
		if (!slot->dst_buf) {
			ret = -ENOMEM;
			goto err_workers;
		}

		if (bch2_verify_compress) {
			slot->verify_buf = kmalloc(extent_max, GFP_KERNEL);
			if (!slot->verify_buf) {
				ret = -ENOMEM;
				goto err_workers;
			}
		}

		slot->gzip_inited = &pool->gzip_inited_arr[i];
		atomic64_set(&slot->state, 0);
		slot->extent_max = extent_max;

		cold->c = c;
		cold->slot = slot;
		cold->expected_gen = 0;
		cold->should_stop = false;
		cold->cpu_id = i;
	}

	c->compress.mt_pool = pool;
	return 0;

err_workers:
	for (unsigned i = 0; i < nr; i++) {
		kvfree(pool->slots[i].workspace);
		kfree(pool->slots[i].src_buf);
		kfree(pool->slots[i].dst_buf);
		kfree(pool->slots[i].verify_buf);
	}
	kfree(pool->gzip_inited_arr);
err_cold:
	pool_free(pool->cold);
err_slots:
	pool_free(pool->slots);
err_pool:
	kfree(pool);
	return ret;
}

void mt_compress_pool_destroy(struct bch_fs *c)
{
	struct mt_compress_pool *pool = c->compress.mt_pool;

	if (!pool)
		return;

	c->compress.mt_pool = NULL;

	if (!pool->workers_started)
		goto free_resources;

	for (unsigned i = 0; i < pool->nr_workers; i++) {
		struct mt_worker_cold *cold = &pool->cold[i];
		struct mt_worker_slot *slot = &pool->slots[i];

		cold->should_stop = true;
		atomic64_set_release(&slot->state, (cold->expected_gen << 1) | 1);
	}

	for (unsigned i = 0; i < pool->nr_workers; i++) {
#ifndef __KERNEL__
		pthread_join(pool->cold[i].thread, NULL);
#else
		kthread_stop(pool->cold[i].task);
#endif
	}

free_resources:

	for (unsigned i = 0; i < pool->nr_workers; i++) {
		kvfree(pool->slots[i].workspace);
		kfree(pool->slots[i].src_buf);
		kfree(pool->slots[i].dst_buf);
		kfree(pool->slots[i].verify_buf);
	}

	kfree(pool->gzip_inited_arr);
	pool_free(pool->cold);
	pool_free(pool->slots);
	kfree(pool);
}

void mt_pool_submit(struct mt_compress_pool *pool,
		    unsigned *worker_ids,
		    size_t *src_lens,
		    struct bpos *positions,
		    unsigned compression_opt,
		    unsigned extent_max,
		    struct mt_completion *completion,
		    unsigned nr)
{
	if (!pool->workers_started) {
		mt_start_workers(pool);
		pool->workers_started = true;
	}

	completion->pool = pool;
	completion->nr = nr;
	atomic_set(&completion->count, nr);
#ifndef __KERNEL__
	atomic_set(&completion->futex_word, 0);
#else
	init_waitqueue_head(&completion->wait);
#endif
	memcpy(completion->worker_ids, worker_ids, nr * sizeof(unsigned));

	for (unsigned i = 0; i < nr; i++) {
		unsigned wid = worker_ids[i];
		struct mt_worker_slot *slot = &pool->slots[wid];
		struct mt_worker_cold *cold = &pool->cold[wid];

		slot->src_len = src_lens[i];
		slot->compression_opt = compression_opt;
		slot->write_pos = positions[i];
		slot->extent_max = extent_max;
		slot->completion = completion;

		atomic64_set_release(&slot->state,
				     (cold->expected_gen << 1) | 1);
	}
}

void mt_pool_wait(struct mt_completion *completion)
{
	completion_wait(completion);
}
