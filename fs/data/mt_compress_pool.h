// SPDX-License-Identifier: GPL-2.0
#ifndef _BCACHEFS_MT_COMPRESS_POOL_H
#define _BCACHEFS_MT_COMPRESS_POOL_H

#include <linux/atomic.h>
#ifdef __KERNEL__
#include <linux/wait.h>
#include <linux/bio.h>
#endif

struct bch_fs;
struct bio;
struct bvec_iter;

#define MT_MAX_WORKERS	32

struct mt_completion;

struct mt_worker_slot {
	void			*src_buf;
	void			*dst_buf;
	void			*workspace;
	void			*verify_buf;
	bool			*gzip_inited;
	size_t			src_len;
	unsigned		compression_opt;
	struct bpos		write_pos;
	unsigned		extent_max;

	atomic64_t		state;
	struct mt_completion	*completion;

	unsigned		compression_type;
	size_t			result_dst_len;
	size_t			result_src_len;
} __attribute__((aligned(128)));

struct mt_worker_cold {
#ifndef __KERNEL__
	pthread_t		thread;
#else
	struct task_struct	*task;
#endif
	struct bch_fs		*c;
	struct mt_worker_slot	*slot;
	u64			expected_gen;
	bool			should_stop;
	unsigned		cpu_id;
} __attribute__((aligned(64)));

struct mt_completion {
	struct mt_compress_pool	*pool;
	unsigned		worker_ids[MT_MAX_WORKERS];
	unsigned		nr;
	atomic_t		count;
#ifndef __KERNEL__
	atomic_t		futex_word;
#else
	wait_queue_head_t	wait;
#endif
} __attribute__((aligned(64)));

struct mt_compress_pool {
	unsigned		nr_workers;
	struct mt_worker_slot	*slots;
	struct mt_worker_cold	*cold;
	bool			*gzip_inited_arr;
	bool			workers_started;
};

int mt_compress_pool_init(struct bch_fs *c);
void mt_compress_pool_destroy(struct bch_fs *c);

void mt_pool_submit(struct mt_compress_pool *pool,
		    unsigned *worker_ids,
		    void **src_ptrs,
		    size_t *src_lens,
		    struct bpos *positions,
		    unsigned compression_opt,
		    unsigned extent_max,
		    struct mt_completion *completion,
		    unsigned nr);

void mt_pool_wait(struct mt_completion *completion);

#endif
