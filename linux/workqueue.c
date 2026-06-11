#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "tools-util.h"

#include <linux/atomic.h>
#include <linux/errname.h>
#include <linux/kthread.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

static pthread_mutex_t	wq_list_lock = PTHREAD_MUTEX_INITIALIZER;
static LIST_HEAD(wq_list);

#define WQ_SHIM_MAX_WORKERS	min(num_online_cpus(), 32U)

struct workqueue_struct {
	struct list_head	list;

	char			name[64];
	unsigned int		max_active;

	struct task_struct	**worker_tasks;
	unsigned int		nr_workers;

	atomic_t		active_count;
	struct work_struct	**worker_current;
	struct list_head	pending;

	pthread_mutex_t		pending_lock;
	pthread_cond_t		*worker_conds;
};

enum {
	WORK_PENDING_BIT,
};

static bool work_pending(struct work_struct *work)
{
	return test_bit(WORK_PENDING_BIT, work_data_bits(work));
}

static void clear_work_pending(struct work_struct *work)
{
	clear_bit(WORK_PENDING_BIT, work_data_bits(work));
}

static bool set_work_pending(struct work_struct *work)
{
	return !test_and_set_bit(WORK_PENDING_BIT, work_data_bits(work));
}

static void wake_nr_workers(struct workqueue_struct *wq, unsigned int nr)
{
	unsigned int woken = 0;

	for (unsigned int i = 0; i < wq->nr_workers && woken < nr; i++) {
		if (wq->worker_current[i] == NULL) {
			pthread_cond_signal(&wq->worker_conds[i]);
			woken++;
		}
	}
}

static int wq_worker_thread(void *arg)
{
	struct workqueue_struct *wq = arg;
	struct work_struct *work;
	unsigned int i;
	struct timespec ts;

	pthread_mutex_lock(&wq->pending_lock);
	for (i = 0; i < wq->nr_workers; i++)
		if (wq->worker_tasks[i] == current)
			break;
	BUG_ON(i == wq->nr_workers);

	{
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);
		CPU_SET(i, &cpuset);
		pthread_setaffinity_np(current->thread, sizeof(cpuset), &cpuset);
	}
	{
		struct sched_param sp = { .sched_priority = 0 };
		pthread_setschedparam(pthread_self(), SCHED_BATCH, &sp);
	}

	while (1) {
		if (kthread_should_stop()) {
			BUG_ON(!list_empty(&wq->pending));
			break;
		}

		work = list_first_entry_or_null(&wq->pending,
				struct work_struct, entry);

		if (!work) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			ts.tv_nsec += 100000000;
			if (ts.tv_nsec >= 1000000000) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&wq->worker_conds[i],
					       &wq->pending_lock, &ts);
			continue;
		}

		BUG_ON(!work_pending(work));
		wq->worker_current[i] = work;
		list_del_init(&work->entry);
		clear_work_pending(work);
		atomic_inc(&wq->active_count);

		pthread_mutex_unlock(&wq->pending_lock);
		work->func(work);

		WRITE_ONCE(wq->worker_current[i], NULL);
		smp_wmb();
		atomic_dec(&wq->active_count);
		pthread_mutex_lock(&wq->pending_lock);
	}
	pthread_mutex_unlock(&wq->pending_lock);

	return 0;
}

static struct task_struct *create_wq_worker(struct workqueue_struct *wq)
{
	struct task_struct *p;
	unsigned int idx = wq->nr_workers;

	if (idx >= wq->max_active)
		return NULL;

	p = kthread_run(wq_worker_thread, wq, "%s/%u", wq->name, idx);
	if (IS_ERR(p))
		return p;

	wq->worker_tasks[idx] = p;
	wq->nr_workers++;
	return p;
}

static void __queue_work(struct workqueue_struct *wq,
			 struct work_struct *work)
{
	BUG_ON(!work_pending(work));
	BUG_ON(!list_empty(&work->entry));

	list_add_tail(&work->entry, &wq->pending);

	if (wq->nr_workers == 0)
		create_wq_worker(wq);

	wake_nr_workers(wq, 1);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	bool ret;

	pthread_mutex_lock(&wq->pending_lock);
	if ((ret = set_work_pending(work)))
		__queue_work(wq, work);
	pthread_mutex_unlock(&wq->pending_lock);

	return ret;
}

unsigned int queue_work_batch(struct workqueue_struct *wq,
			     struct work_struct **works,
			     unsigned int count)
{
	unsigned int queued = 0;

	if (!count)
		return 0;

	pthread_mutex_lock(&wq->pending_lock);

	for (unsigned int i = 0; i < count; i++) {
		if (!set_work_pending(works[i]))
			continue;
		BUG_ON(!list_empty(&works[i]->entry));
		list_add_tail(&works[i]->entry, &wq->pending);
		queued++;
	}

	if (queued) {
		if (wq->nr_workers == 0)
			create_wq_worker(wq);
		wake_nr_workers(wq, queued);
	}

	pthread_mutex_unlock(&wq->pending_lock);
	return queued;
}

void delayed_work_timer_fn(struct timer_list *timer)
{
	struct delayed_work *dwork =
		container_of(timer, struct delayed_work, timer);

	pthread_mutex_lock(&dwork->wq->pending_lock);
	__queue_work(dwork->wq, &dwork->work);
	pthread_mutex_unlock(&dwork->wq->pending_lock);
}

static void __queue_delayed_work(struct workqueue_struct *wq,
				 struct delayed_work *dwork,
				 unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;

	BUG_ON(timer->function != delayed_work_timer_fn);
	BUG_ON(timer_pending(timer));
	BUG_ON(!list_empty(&work->entry));

	if (!delay) {
		__queue_work(wq, &dwork->work);
	} else {
		dwork->wq = wq;
		timer->expires = jiffies + delay;
		add_timer(timer);
	}
}

bool queue_delayed_work(struct workqueue_struct *wq,
			struct delayed_work *dwork,
			unsigned long delay)
{
	struct work_struct *work = &dwork->work;
	bool ret;

	pthread_mutex_lock(&wq->pending_lock);
	if ((ret = set_work_pending(work)))
		__queue_delayed_work(wq, dwork, delay);
	pthread_mutex_unlock(&wq->pending_lock);

	return ret;
}

static bool grab_pending(struct work_struct *work, bool is_dwork)
{
retry:
	if (set_work_pending(work)) {
		BUG_ON(!list_empty(&work->entry));
		return false;
	}

	if (is_dwork) {
		struct delayed_work *dwork = to_delayed_work(work);

		if (likely(del_timer(&dwork->timer))) {
			BUG_ON(!list_empty(&work->entry));
			return true;
		}
	}

	if (!list_empty(&work->entry)) {
		list_del_init(&work->entry);
		return true;
	}

	BUG_ON(!is_dwork);

	pthread_mutex_unlock(&wq_list_lock);
	flush_timers();
	pthread_mutex_lock(&wq_list_lock);
	goto retry;
}

static bool work_running(struct work_struct *work)
{
	struct workqueue_struct *wq;
	unsigned int i;

	list_for_each_entry(wq, &wq_list, list) {
		for (i = 0; i < wq->nr_workers; i++)
			if (READ_ONCE(wq->worker_current[i]) == work)
				return true;
	}

	return false;
}

bool flush_work(struct work_struct *work)
{
	bool ret = false;

	while (work_pending(work) || work_running(work)) {
		schedule();
		ret = true;
	}

	return ret;
}

static bool __flush_work(struct work_struct *work)
{
	bool ret = false;

	while (work_running(work)) {
		schedule();
		ret = true;
	}

	return ret;
}

bool cancel_work_sync(struct work_struct *work)
{
	bool ret;

	pthread_mutex_lock(&wq_list_lock);
	ret = grab_pending(work, false);

	__flush_work(work);
	clear_work_pending(work);
	pthread_mutex_unlock(&wq_list_lock);

	return ret;
}

bool mod_delayed_work(struct workqueue_struct *wq,
		      struct delayed_work *dwork,
		      unsigned long delay)
{
	struct work_struct *work = &dwork->work;
	bool ret;

	pthread_mutex_lock(&wq->pending_lock);
	ret = grab_pending(work, true);

	__queue_delayed_work(wq, dwork, delay);
	pthread_mutex_unlock(&wq->pending_lock);

	return ret;
}

bool cancel_delayed_work(struct delayed_work *dwork)
{
	struct work_struct *work = &dwork->work;
	bool ret;

	pthread_mutex_lock(&wq_list_lock);
	ret = grab_pending(work, true);

	clear_work_pending(&dwork->work);
	pthread_mutex_unlock(&wq_list_lock);

	return ret;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	struct work_struct *work = &dwork->work;
	bool ret;

	pthread_mutex_lock(&wq_list_lock);
	ret = grab_pending(work, true);

	__flush_work(work);
	clear_work_pending(work);
	pthread_mutex_unlock(&wq_list_lock);

	return ret;
}

void drain_workqueue(struct workqueue_struct *wq)
{
	while (1) {
		pthread_mutex_lock(&wq->pending_lock);
		bool empty = list_empty(&wq->pending);
		pthread_mutex_unlock(&wq->pending_lock);

		if (empty && atomic_read(&wq->active_count) == 0)
			break;

		schedule();
	}
}

void flush_workqueue(struct workqueue_struct *wq)
{
	drain_workqueue(wq);
}

void destroy_workqueue(struct workqueue_struct *wq)
{
	struct task_struct **tasks;
	unsigned int n;

	pthread_mutex_lock(&wq_list_lock);
	list_del(&wq->list);
	pthread_mutex_unlock(&wq_list_lock);

	tasks = wq->worker_tasks;
	n = wq->nr_workers;

	pthread_mutex_lock(&wq->pending_lock);
	for (unsigned i = 0; i < n; i++)
		pthread_cond_signal(&wq->worker_conds[i]);
	pthread_mutex_unlock(&wq->pending_lock);

	for (unsigned i = 0; i < n; i++) {
		kthread_stop(tasks[i]);
		put_task_struct(tasks[i]);
	}

	for (unsigned i = 0; i < wq->max_active; i++)
		pthread_cond_destroy(&wq->worker_conds[i]);
	free(wq->worker_conds);
	free(wq->worker_current);
	free(tasks);
	pthread_mutex_destroy(&wq->pending_lock);
	kfree(wq);
}

static unsigned int compute_nr_workers(int max_active)
{
	unsigned int nr;

	if (max_active <= 0)
		nr = 1;
	else if ((unsigned int)max_active > WQ_MAX_ACTIVE)
		nr = WQ_SHIM_MAX_WORKERS;
	else
		nr = (unsigned int)max_active;

	if (nr > WQ_SHIM_MAX_WORKERS)
		nr = WQ_SHIM_MAX_WORKERS;
	return nr;
}

struct workqueue_struct *alloc_workqueue(const char *fmt,
					 unsigned flags,
					 int max_active,
					 ...)
{
	va_list args;
	struct workqueue_struct *wq;

	wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	if (!wq)
		return NULL;

	INIT_LIST_HEAD(&wq->pending);
	atomic_set(&wq->active_count, 0);
	wq->max_active = compute_nr_workers(max_active);
	pthread_mutex_init(&wq->pending_lock, NULL);

	va_start(args, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	va_end(args);

	wq->worker_tasks = calloc(wq->max_active, sizeof(struct task_struct *));
	if (!wq->worker_tasks) {
		kfree(wq);
		return NULL;
	}

	wq->worker_current = calloc(wq->max_active, sizeof(struct work_struct *));
	if (!wq->worker_current) {
		free(wq->worker_tasks);
		kfree(wq);
		return NULL;
	}

	wq->worker_conds = calloc(wq->max_active, sizeof(pthread_cond_t));
	if (!wq->worker_conds) {
		free(wq->worker_current);
		free(wq->worker_tasks);
		kfree(wq);
		return NULL;
	}

	for (unsigned i = 0; i < wq->max_active; i++)
		pthread_cond_init(&wq->worker_conds[i], NULL);

	pthread_mutex_lock(&wq_list_lock);
	list_add(&wq->list, &wq_list);
	pthread_mutex_unlock(&wq_list_lock);

	return wq;
}

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_freezable_wq;

__attribute__((constructor(102)))
static void wq_init(void)
{
	system_wq = alloc_workqueue("events", 0, 0);
	system_highpri_wq = alloc_workqueue("events_highpri", WQ_HIGHPRI, 0);
	system_long_wq = alloc_workqueue("events_long", 0, 0);
	system_unbound_wq = alloc_workqueue("events_unbound", WQ_UNBOUND,
					    WQ_UNBOUND_MAX_ACTIVE);
	system_freezable_wq = alloc_workqueue("events_freezable",
					      WQ_FREEZABLE, 0);
	BUG_ON(!system_wq || !system_highpri_wq || !system_long_wq ||
	       !system_unbound_wq || !system_freezable_wq);
}
