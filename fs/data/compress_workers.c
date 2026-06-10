// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"

#include "data/compress.h"
#include "data/compress_workers.h"

#include <linux/lz4.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/zlib.h>
#include <linux/zstd.h>

static void bch2_compress_work_fn(struct work_struct *work)
{
	struct bch_compress_work *w =
		container_of(work, struct bch_compress_work, work);

	w->compression_type = bch2_compress_locked(
		w->cwq->c,
		w->dst, &w->dst_len,
		(void *) w->src, &w->src_len,
		w->compression_opt,
		w->write_pos,
		w->worker->workspace,
		w->worker->verify_buf,
		false,
		&w->worker->gzip_inited);

	if (w->nr_pending) {
		smp_wmb();
		atomic_dec(w->nr_pending);
	}

	atomic_set_release(&w->done, 1);
}

int bch2_compress_wq_init(struct bch_fs *c)
{
	unsigned nr = bch2_compress_nr_workers();
	struct bch_compress_wq *cwq;
	int ret = 0;

	cwq = kzalloc(sizeof(*cwq), GFP_KERNEL);
	if (!cwq)
		return -ENOMEM;

	cwq->c = c;
	cwq->nr_workers = nr;

	cwq->wq = alloc_workqueue("bcachefs-compress",
				  WQ_UNBOUND | WQ_HIGHPRI, nr);
	if (!cwq->wq) {
		ret = -ENOMEM;
		goto err_free_cwq;
	}

	cwq->workers = kcalloc(nr, sizeof(*cwq->workers), GFP_KERNEL);
	if (!cwq->workers) {
		ret = -ENOMEM;
		goto err_destroy_wq;
	}

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
	unsigned extent_max = c->opts.encoded_extent_max;

	for (unsigned i = 0; i < nr; i++) {
		struct bch_compress_worker *worker = &cwq->workers[i];

		worker->workspace = kvmalloc(ws_size, GFP_KERNEL);
		if (!worker->workspace) {
			ret = -ENOMEM;
			goto err_free_workers;
		}

		worker->dst_buf = kvmalloc(extent_max, GFP_KERNEL);
		if (!worker->dst_buf) {
			ret = -ENOMEM;
			goto err_free_workers;
		}

		if (bch2_verify_compress)
			worker->verify_buf = kvmalloc(extent_max, GFP_KERNEL);
		if (!worker->verify_buf && bch2_verify_compress) {
			ret = -ENOMEM;
			goto err_free_workers;
		}

		worker->src_buf = kvmalloc(extent_max, GFP_KERNEL);
		if (!worker->src_buf) {
			ret = -ENOMEM;
			goto err_free_workers;
		}
	}

	c->compress.mt_wq = cwq;
	return 0;
err_free_workers:
	for (unsigned i = 0; i < nr; i++) {
		kvfree(cwq->workers[i].workspace);
		kvfree(cwq->workers[i].dst_buf);
		kvfree(cwq->workers[i].verify_buf);
		kvfree(cwq->workers[i].src_buf);
	}
	kfree(cwq->workers);
err_destroy_wq:
	destroy_workqueue(cwq->wq);
err_free_cwq:
	kfree(cwq);
	return ret;
}

void bch2_compress_wq_destroy(struct bch_fs *c)
{
	struct bch_compress_wq *cwq = c->compress.mt_wq;

	if (!cwq)
		return;

	c->compress.mt_wq = NULL;

	for (unsigned i = 0; i < cwq->nr_workers; i++) {
		kvfree(cwq->workers[i].workspace);
		kvfree(cwq->workers[i].dst_buf);
		kvfree(cwq->workers[i].verify_buf);
		kvfree(cwq->workers[i].src_buf);
	}
	kfree(cwq->workers);
	destroy_workqueue(cwq->wq);
	kfree(cwq);
}

void bch2_compress_wq_submit(struct bch_compress_work *w,
			     struct bch_compress_wq *cwq,
			     unsigned compression_opt,
			     struct bpos write_pos,
			     const void *src, size_t src_len,
			     void *dst, size_t dst_len,
			     struct bch_compress_worker *worker)
{
	INIT_WORK(&w->work, bch2_compress_work_fn);
	w->cwq = cwq;
	w->compression_opt = compression_opt;
	w->write_pos = write_pos;
	w->src = src;
	w->src_len = src_len;
	w->dst = dst;
	w->dst_len = dst_len;
	w->compression_type = 0;
	w->worker = worker;
	w->nr_pending = NULL;
	atomic_set(&w->done, 0);

	queue_work(cwq->wq, &w->work);
}

void bch2_compress_wq_prepare(struct bch_compress_work *w,
			      struct bch_compress_wq *cwq,
			      unsigned compression_opt,
			      struct bpos write_pos,
			      const void *src, size_t src_len,
			      void *dst, size_t dst_len,
			      struct bch_compress_worker *worker,
			      atomic_t *nr_pending)
{
	INIT_WORK(&w->work, bch2_compress_work_fn);
	w->cwq = cwq;
	w->compression_opt = compression_opt;
	w->write_pos = write_pos;
	w->src = src;
	w->src_len = src_len;
	w->dst = dst;
	w->dst_len = dst_len;
	w->compression_type = 0;
	w->worker = worker;
	w->nr_pending = nr_pending;
	atomic_set(&w->done, 0);
}

void bch2_compress_wq_submit_batch(struct bch_compress_wq *cwq,
				   struct bch_compress_work *works,
				   unsigned nr)
{
#ifndef __KERNEL__
	struct work_struct *ptrs[32];

	BUG_ON(nr > 32);

	for (unsigned i = 0; i < nr; i++)
		ptrs[i] = &works[i].work;

	queue_work_batch(cwq->wq, ptrs, nr);
#else
	for (unsigned i = 0; i < nr; i++)
		queue_work(cwq->wq, &works[i].work);
#endif
}

static void yield_cpu(void)
{
#ifndef __KERNEL__
	sched_yield();
#else
	cond_resched();
#endif
}

void bch2_compress_wq_wait_all(struct bch_compress_work *works, unsigned nr)
{
	unsigned spins = 0;

	while (1) {
		bool all_done = true;

		for (unsigned i = 0; i < nr; i++) {
			if (!atomic_read_acquire(&works[i].done)) {
				all_done = false;
				break;
			}
		}
		if (all_done)
			return;

		cpu_relax();
		if (++spins >= 1000) {
			yield_cpu();
			spins = 0;
		}
	}
}

void bch2_compress_wq_wait_atomic(atomic_t *counter, unsigned val)
{
	unsigned spins = 0;

	while ((unsigned)atomic_read_acquire(counter) > val) {
		cpu_relax();
		if (++spins >= 1000) {
			yield_cpu();
			spins = 0;
		}
	}
}
