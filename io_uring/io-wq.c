// SPDX-License-Identifier: GPL-2.0
/*
 * Basic worker thread pool for io_uring
 *
 * Copyright (C) 2019 Jens Axboe
 *
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/sched/signal.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/rculist_nulls.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/task_work.h>
#include <linux/audit.h>
#include <linux/mmu_context.h>
#include <uapi/linux/io_uring.h>

#include "io-wq.h"
#include "slist.h"
#include "io_uring.h"

#define WORKER_IDLE_TIMEOUT	(5 * HZ)
#define WORKER_INIT_LIMIT	3

enum {
	IO_WORKER_F_UP		= 0,	/* up and active */
	IO_WORKER_F_RUNNING	= 1,	/* account as running */
	IO_WORKER_F_FREE	= 2,	/* worker on free list */
};

enum {
	IO_WQ_BIT_EXIT		= 0,	/* wq exiting */
};

enum {
	IO_ACCT_STALLED_BIT	= 0,	/* stalled on hash */
};

/*
 * One for each thread in a wq pool
 */
struct io_worker {
	refcount_t ref;
	unsigned long flags;
	struct hlist_nulls_node nulls_node;
	struct list_head all_list;
	struct task_struct *task;
	struct io_wq *wq;
	struct io_wq_acct *acct;

	struct io_wq_work *cur_work;
	raw_spinlock_t lock;

	struct completion ref_done;

	unsigned long create_state;
	struct callback_head create_work;
	int init_retries;

	union {
		struct rcu_head rcu;
		struct delayed_work work;
	};
};

#if BITS_PER_LONG == 64
#define IO_WQ_HASH_ORDER	6
#else
#define IO_WQ_HASH_ORDER	5
#endif

#define IO_WQ_NR_HASH_BUCKETS	(1u << IO_WQ_HASH_ORDER)

struct io_wq_acct {
	/**
	 * Protects access to the worker lists.
	 */
	raw_spinlock_t workers_lock;

	unsigned nr_workers;
	unsigned max_workers;
	atomic_t nr_running;

	/**
	 * The list of free workers.  Protected by #workers_lock
	 * (write) and RCU (read).
	 */
	struct hlist_nulls_head free_list;

	/**
	 * The list of all workers.  Protected by #workers_lock
	 * (write) and RCU (read).
	 */
	struct list_head all_list;

	raw_spinlock_t lock;
	struct io_wq_work_list work_list;
	unsigned long flags;
};

enum {
	IO_WQ_ACCT_BOUND,
	IO_WQ_ACCT_UNBOUND,
	IO_WQ_ACCT_NR,
};

/*
 * Per io_wq state
  */
struct io_wq {
	unsigned long state;

	struct io_wq_hash *hash;

	atomic_t worker_refs;
	struct completion worker_done;

	struct hlist_node cpuhp_node;

	struct task_struct *task;

	struct io_wq_acct acct[IO_WQ_ACCT_NR];

	struct wait_queue_entry wait;

	struct io_wq_work *hash_tail[IO_WQ_NR_HASH_BUCKETS];

	cpumask_var_t cpu_mask;
};

static enum cpuhp_state io_wq_online;

struct io_cb_cancel_data {
	work_cancel_fn *fn;
	void *data;
	int nr_running;
	int nr_pending;
	bool cancel_all;
};

static bool create_io_worker(struct io_wq *wq, struct io_wq_acct *acct);
static void io_wq_dec_running(struct io_worker *worker);
static bool io_acct_cancel_pending_work(struct io_wq *wq,
					struct io_wq_acct *acct,
					struct io_cb_cancel_data *match);
static void create_worker_cb(struct callback_head *cb);
static void io_wq_cancel_tw_create(struct io_wq *wq);

static inline unsigned int __io_get_work_hash(unsigned int work_flags)
{
	return work_flags >> IO_WQ_HASH_SHIFT;
}

static inline unsigned int io_get_work_hash(struct io_wq_work *work)
{
	return __io_get_work_hash(atomic_read(&work->flags));
}

static bool io_worker_get(struct io_worker *worker)
{
	return refcount_inc_not_zero(&worker->ref);
}

static void io_worker_release(struct io_worker *worker)
{
	if (refcount_dec_and_test(&worker->ref))
		complete(&worker->ref_done);
}

static inline struct io_wq_acct *io_get_acct(struct io_wq *wq, bool bound)
{
	return &wq->acct[bound ? IO_WQ_ACCT_BOUND : IO_WQ_ACCT_UNBOUND];
}

static inline struct io_wq_acct *io_work_get_acct(struct io_wq *wq,
						  unsigned int work_flags)
{
	return io_get_acct(wq, !(work_flags & IO_WQ_WORK_UNBOUND));
}

static inline struct io_wq_acct *io_wq_get_acct(struct io_worker *worker)
{
	return worker->acct;
}

static void io_worker_ref_put(struct io_wq *wq)
{
	if (atomic_dec_and_test(&wq->worker_refs))
		complete(&wq->worker_done);
}

bool io_wq_worker_stopped(void)
{
	struct io_worker *worker = current->worker_private;

	if (WARN_ON_ONCE(!io_wq_current_is_worker()))
		return true;

	return test_bit(IO_WQ_BIT_EXIT, &worker->wq->state);
}

static void io_worker_cancel_cb(struct io_worker *worker)
{
	struct io_wq_acct *acct = io_wq_get_acct(worker);
	struct io_wq *wq = worker->wq;

	atomic_dec(&acct->nr_running);
	raw_spin_lock(&acct->workers_lock);
	acct->nr_workers--;
	raw_spin_unlock(&acct->workers_lock);
	io_worker_ref_put(wq);
	clear_bit_unlock(0, &worker->create_state);
	io_worker_release(worker);
}

static bool io_task_worker_match(struct callback_head *cb, void *data)
{
	struct io_worker *worker;

	if (cb->func != create_worker_cb)
		return false;
	worker = container_of(cb, struct io_worker, create_work);
	return worker == data;
}

static void io_worker_exit(struct io_worker *worker)
{
	struct io_wq *wq = worker->wq;
	struct io_wq_acct *acct = io_wq_get_acct(worker);

	while (1) {
		struct callback_head *cb = task_work_cancel_match(wq->task,
						io_task_worker_match, worker);

		if (!cb)
			break;
		io_worker_cancel_cb(worker);
	}

	io_worker_release(worker);
	wait_for_completion(&worker->ref_done);

	raw_spin_lock(&acct->workers_lock);
	if (test_bit(IO_WORKER_F_FREE, &worker->flags))
		hlist_nulls_del_rcu(&worker->nulls_node);
	list_del_rcu(&worker->all_list);
	raw_spin_unlock(&acct->workers_lock);
	io_wq_dec_running(worker);
	/*
	 * this worker is a goner, clear ->worker_private to avoid any
	 * inc/dec running calls that could happen as part of exit from
	 * touching 'worker'.
	 */
	current->worker_private = NULL;

	kfree_rcu(worker, rcu);
	io_worker_ref_put(wq);
	do_exit(0);
}

static inline bool __io_acct_run_queue(struct io_wq_acct *acct)
{
	return !test_bit(IO_ACCT_STALLED_BIT, &acct->flags) &&
		!wq_list_empty(&acct->work_list);
}

/*
 * If there's work to do, returns true with acct->lock acquired. If not,
 * returns false with no lock held.
 */
static inline bool io_acct_run_queue(struct io_wq_acct *acct)
	__acquires(&acct->lock)
{
	raw_spin_lock(&acct->lock);
	if (__io_acct_run_queue(acct))
		return true;

	raw_spin_unlock(&acct->lock);
	return false;
}

/*
 * Check head of free list for an available worker. If one isn't available,
 * caller must create one.
 */
static bool io_acct_activate_free_worker(struct io_wq_acct *acct)
	__must_hold(RCU)
{
	struct hlist_nulls_node *n;
	struct io_worker *worker;

	/*
	 * Iterate free_list and see if we can find an idle worker to
	 * activate. If a given worker is on the free_list but in the process
	 * of exiting, keep trying.
	 */
	hlist_nulls_for_each_entry_rcu(worker, n, &acct->free_list, nulls_node) {
		if (!io_worker_get(worker))
			continue;
		/*
		 * If the worker is already running, it's either already
		 * starting work or finishing work. In either case, if it does
		 * to go sleep, we'll kick off a new task for this work anyway.
		 */
		wake_up_process(worker->task);
		io_worker_release(worker);
		return true;
	}

	return false;
}

/*
 * We need a worker. If we find a free one, we're good. If not, and we're
 * below the max number of workers, create one.
 */
static bool io_wq_create_worker(struct io_wq *wq, struct io_wq_acct *acct)
{
	/*
	 * Most likely an attempt to queue unbounded work on an io_wq that
	 * wasn't setup with any unbounded workers.
	 */
	if (unlikely(!acct->max_workers))
		pr_warn_once("io-wq is not configured for unbound workers");

	raw_spin_lock(&acct->workers_lock);
	if (acct->nr_workers >= acct->max_workers) {
		raw_spin_unlock(&acct->workers_lock);
		return true;
	}
	acct->nr_workers++;
	raw_spin_unlock(&acct->workers_lock);
	atomic_inc(&acct->nr_running);
	atomic_inc(&wq->worker_refs);
	return create_io_worker(wq, acct);
}

static void io_wq_inc_running(struct io_worker *worker)
{
	struct io_wq_acct *acct = io_wq_get_acct(worker);

	atomic_inc(&acct->nr_running);
}

static void create_worker_cb(struct callback_head *cb)
{
	struct io_worker *worker;
	struct io_wq *wq;

	struct io_wq_acct *acct;
	bool do_create = false;

	worker = container_of(cb, struct io_worker, create_work);
	wq = worker->wq;
	acct = worker->acct;
	raw_spin_lock(&acct->workers_lock);

	if (acct->nr_workers < acct->max_workers) {
		acct->nr_workers++;
		do_create = true;
	}
	raw_spin_unlock(&acct->workers_lock);
	if (do_create) {
		create_io_worker(wq, acct);
	} else {
		atomic_dec(&acct->nr_running);
		io_worker_ref_put(wq);
	}
	clear_bit_unlock(0, &worker->create_state);
	io_worker_release(worker);
}

static bool io_queue_worker_create(struct io_worker *worker,
				   struct io_wq_acct *acct,
				   task_work_func_t func)
{
	struct io_wq *wq = worker->wq;

	/* raced with exit, just ignore create call */
	if (test_bit(IO_WQ_BIT_EXIT, &wq->state))
		goto fail;
	if (!io_worker_get(worker))
		goto fail;
	/*
	 * create_state manages ownership of create_work/index. We should
	 * only need one entry per worker, as the worker going to sleep
	 * will trigger the condition, and waking will clear it once it
	 * runs the task_work.
	 */
	if (test_bit(0, &worker->create_state) ||
	    test_and_set_bit_lock(0, &worker->create_state))
		goto fail_release;

	atomic_inc(&wq->worker_refs);
	init_task_work(&worker->create_work, func);
	if (!task_work_add(wq->task, &worker->create_work, TWA_SIGNAL)) {
		/*
		 * EXIT may have been set after checking it above, check after
		 * adding the task_work and remove any creation item if it is
		 * now set. wq exit does that too, but we can have added this
		 * work item after we canceled in io_wq_exit_workers().
		 */
		if (test_bit(IO_WQ_BIT_EXIT, &wq->state))
			io_wq_cancel_tw_create(wq);
		io_worker_ref_put(wq);
		return true;
	}
	io_worker_ref_put(wq);
	clear_bit_unlock(0, &worker->create_state);
fail_release:
	io_worker_release(worker);
fail:
	atomic_dec(&acct->nr_running);
	io_worker_ref_put(wq);
	return false;
}

/* Defer if current and next work are both hashed to the same chain */
static bool io_wq_hash_defer(struct io_wq_work *work, struct io_wq_acct *acct)
{
	unsigned int hash, work_flags;
	struct io_wq_work *next;

	lockdep_assert_held(&acct->lock);

	work_flags = atomic_read(&work->flags);
	if (!__io_wq_is_hashed(work_flags))
		return false;

	/* should not happen, io_acct_run_queue() said we had work */
	if (wq_list_empty(&acct->work_list))
		return true;

	hash = __io_get_work_hash(work_flags);
	next = container_of(acct->work_list.first, struct io_wq_work, list);
	work_flags = atomic_read(&next->flags);
	if (!__io_wq_is_hashed(work_flags))
		return false;
	return hash == __io_get_work_hash(work_flags);
}

static void io_wq_dec_running(struct io_worker *worker)
{
	struct io_wq_acct *acct = io_wq_get_acct(worker);
	struct io_wq *wq = worker->wq;

	if (!test_bit(IO_WORKER_F_UP, &worker->flags))
		return;

	if (!atomic_dec_and_test(&acct->nr_running))
		return;
	if (!worker->cur_work)
		return;
	if (!io_acct_run_queue(acct))
		return;
	if (io_wq_hash_defer(worker->cur_work, acct)) {
		raw_spin_unlock(&acct->lock);
		return;
	}

	raw_spin_unlock(&acct->lock);
	atomic_inc(&acct->nr_running);
	atomic_inc(&wq->worker_refs);
	io_queue_worker_create(worker, acct, create_worker_cb);
}

/*
 * Worker will start processing some work. Move it to the busy list, if
 * it's currently on the freelist
 */
static void __io_worker_busy(struct io_wq_acct *acct, struct io_worker *worker)
{
	if (test_bit(IO_WORKER_F_FREE, &worker->flags)) {
		clear_bit(IO_WORKER_F_FREE, &worker->flags);
		raw_spin_lock(&acct->workers_lock);
		hlist_nulls_del_init_rcu(&worker->nulls_node);
		raw_spin_unlock(&acct->workers_lock);
	}
}

/*
 * No work, worker going to sleep. Move to freelist.
 */
static void __io_worker_idle(struct io_wq_acct *acct, struct io_worker *worker)
	__must_hold(acct->workers_lock)
{
	if (!test_bit(IO_WORKER_F_FREE, &worker->flags)) {
		set_bit(IO_WORKER_F_FREE, &worker->flags);
		hlist_nulls_add_head_rcu(&worker->nulls_node, &acct->free_list);
	}
}

static bool io_wait_on_hash(struct io_wq *wq, unsigned int hash)
{
	bool ret = false;

	spin_lock_irq(&wq->hash->wait.lock);
	if (list_empty(&wq->wait.entry)) {
		__add_wait_queue(&wq->hash->wait, &wq->wait);
		if (!test_bit(hash, &wq->hash->map)) {
			__set_current_state(TASK_RUNNING);
			list_del_init(&wq->wait.entry);
			ret = true;
		}
	}
	spin_unlock_irq(&wq->hash->wait.lock);
	return ret;
}

static struct io_wq_work *io_get_next_work(struct io_wq_acct *acct,
					   struct io_wq *wq)
	__must_hold(acct->lock)
{
	struct io_wq_work_node *node, *prev;
	struct io_wq_work *work, *tail;
	unsigned int stall_hash = -1U;

	wq_list_for_each(node, prev, &acct->work_list) {
		unsigned int work_flags;
		unsigned int hash;

		work = container_of(node, struct io_wq_work, list);

		/* not hashed, can run anytime */
		work_flags = atomic_read(&work->flags);
		if (!__io_wq_is_hashed(work_flags)) {
			wq_list_del(&acct->work_list, node, prev);
			return work;
		}

		hash = __io_get_work_hash(work_flags);
		/* all items with this hash lie in [work, tail] */
		tail = wq->hash_tail[hash];

		/* hashed, can run if not already running */
		if (!test_and_set_bit(hash, &wq->hash->map)) {
			wq->hash_tail[hash] = NULL;
			wq_list_cut(&acct->work_list, &tail->list, prev);
			return work;
		}
		if (stall_hash == -1U)
			stall_hash = hash;
		/* fast forward to a next hash, for-each will fix up @prev */
		node = &tail->list;
	}

	if (stall_hash != -1U) {
		bool unstalled;

		/*
		 * Set this before dropping the lock to avoid racing with new
		 * work being added and clearing the stalled bit.
		 */
		set_bit(IO_ACCT_STALLED_BIT, &acct->flags);
		raw_spin_unlock(&acct->lock);
		unstalled = io_wait_on_hash(wq, stall_hash);
		raw_spin_lock(&acct->lock);
		if (unstalled) {
			clear_bit(IO_ACCT_STALLED_BIT, &acct->flags);
			if (wq_has_sleeper(&wq->hash->wait))
				wake_up(&wq->hash->wait);
		}
	}

	return NULL;
}

static void io_assign_current_work(struct io_worker *worker,
				   struct io_wq_work *work)
{
	if (work) {
		io_run_task_work();
		cond_resched();
	}

	raw_spin_lock(&worker->lock);
	worker->cur_work = work;
	raw_spin_unlock(&worker->lock);
}

/*
 * Called with acct->lock held, drops it before returning
 */
static void io_worker_handle_work(struct io_wq_acct *acct,
				  struct io_worker *worker)
	__releases(&acct->lock)
{
	struct io_wq *wq = worker->wq;
	bool do_kill = test_bit(IO_WQ_BIT_EXIT, &wq->state);

	do {
		struct io_wq_work *work;

		/*
		 * If we got some work, mark us as busy. If we didn't, but
		 * the list isn't empty, it means we stalled on hashed work.
		 * Mark us stalled so we don't keep looking for work when we
		 * can't make progress, any work completion or insertion will
		 * clear the stalled flag.
		 */
		work = io_get_next_work(acct, wq);
		if (work) {
			/*
			 * Make sure cancelation can find this, even before
			 * it becomes the active work. That avoids a window
			 * where the work has been removed from our general
			 * work list, but isn't yet discoverable as the
			 * current work item for this worker.
			 */
			raw_spin_lock(&worker->lock);
			worker->cur_work = work;
			raw_spin_unlock(&worker->lock);
		}

		raw_spin_unlock(&acct->lock);

		if (!work)
			break;

		__io_worker_busy(acct, worker);

		io_assign_current_work(worker, work);
		__set_current_state(TASK_RUNNING);

		/* handle a whole dependent link */
		do {
			struct io_wq_work *next_hashed, *linked;
			unsigned int work_flags = atomic_read(&work->flags);
			unsigned int hash = __io_wq_is_hashed(work_flags)
				? __io_get_work_hash(work_flags)
				: -1U;

			next_hashed = wq_next_work(work);

			if (do_kill &&
			    (work_flags & IO_WQ_WORK_UNBOUND))
				atomic_or(IO_WQ_WORK_CANCEL, &work->flags);
			io_wq_submit_work(work);
			io_assign_current_work(worker, NULL);

			linked = io_wq_free_work(work);
			work = next_hashed;
			if (!work && linked && !io_wq_is_hashed(linked)) {
				work = linked;
				linked = NULL;
			}
			io_assign_current_work(worker, work);
			if (linked)
				io_wq_enqueue(wq, linked);

			if (hash != -1U && !next_hashed) {
				/* serialize hash clear with wake_up() */
				spin_lock_irq(&wq->hash->wait.lock);
				clear_bit(hash, &wq->hash->map);
				clear_bit(IO_ACCT_STALLED_BIT, &acct->flags);
				spin_unlock_irq(&wq->hash->wait.lock);
				if (wq_has_sleeper(&wq->hash->wait))
					wake_up(&wq->hash->wait);
			}
		} while (work);

		if (!__io_acct_run_queue(acct))
			break;
		raw_spin_lock(&acct->lock);
	} while (1);
}

static int io_wq_worker(void *data)
{
	struct io_worker *worker = data;
	struct io_wq_acct *acct = io_wq_get_acct(worker);
	struct io_wq *wq = worker->wq;
	bool exit_mask = false, last_timeout = false;
	char buf[TASK_COMM_LEN] = {};

	set_mask_bits(&worker->flags, 0,
		      BIT(IO_WORKER_F_UP) | BIT(IO_WORKER_F_RUNNING));

	snprintf(buf, sizeof(buf), "iou-wrk-%d", wq->task->pid);
	set_task_comm(current, buf);

	while (!test_bit(IO_WQ_BIT_EXIT, &wq->state)) {
		long ret;

		set_current_state(TASK_INTERRUPTIBLE);

		/*
		 * If we have work to do, io_acct_run_queue() returns with
		 * the acct->lock held. If not, it will drop it.
		 */
		while (io_acct_run_queue(acct))
			io_worker_handle_work(acct, worker);

		raw_spin_lock(&acct->workers_lock);
		/*
		 * Last sleep timed out. Exit if we're not the last worker,
		 * or if someone modified our affinity.
		 */
		if (last_timeout && (exit_mask || acct->nr_workers > 1)) {
			acct->nr_workers--;
			raw_spin_unlock(&acct->workers_lock);
			__set_current_state(TASK_RUNNING);
			break;
		}
		last_timeout = false;
		__io_worker_idle(acct, worker);
		raw_spin_unlock(&acct->workers_lock);
		if (io_run_task_work())
			continue;
		ret = schedule_timeout(WORKER_IDLE_TIMEOUT);
		if (signal_pending(current)) {
			struct ksignal ksig;

			if (!get_signal(&ksig))
				continue;
			break;
		}
		if (!ret) {
			last_timeout = true;
			exit_mask = !cpumask_test_cpu(raw_smp_processor_id(),
							wq->cpu_mask);
		}
	}

	if (test_bit(IO_WQ_BIT_EXIT, &wq->state) && io_acct_run_queue(acct))
		io_worker_handle_work(acct, worker);

	io_worker_exit(worker);
	return 0;
}

/*
 * Called when a worker is scheduled in. Mark us as currently running.
 */
void io_wq_worker_running(struct task_struct *tsk)
{
	struct io_worker *worker = tsk->worker_private;

	if (!worker)
		return;
	if (!test_bit(IO_WORKER_F_UP, &worker->flags))
		return;
	if (test_bit(IO_WORKER_F_RUNNING, &worker->flags))
		return;
	set_bit(IO_WORKER_F_RUNNING, &worker->flags);
	io_wq_inc_running(worker);
}

/*
 * Called when worker is going to sleep. If there are no workers currently
 * running and we have work pending, wake up a free one or create a new one.
 */
void io_wq_worker_sleeping(struct task_struct *tsk)
{
	struct io_worker *worker = tsk->worker_private;

	if (!worker)
		return;
	if (!test_bit(IO_WORKER_F_UP, &worker->flags))
		return;
	if (!test_bit(IO_WORKER_F_RUNNING, &worker->flags))
		return;

	clear_bit(IO_WORKER_F_RUNNING, &worker->flags);
	io_wq_dec_running(worker);
}

static void io_init_new_worker(struct io_wq *wq, struct io_wq_acct *acct, struct io_worker *worker,
			       struct task_struct *tsk)
{
	tsk->worker_private = worker;
	worker->task = tsk;
	set_cpus_allowed_ptr(tsk, wq->cpu_mask);

	raw_spin_lock(&acct->workers_lock);
	hlist_nulls_add_head_rcu(&worker->nulls_node, &acct->free_list);
	list_add_tail_rcu(&worker->all_list, &acct->all_list);
	set_bit(IO_WORKER_F_FREE, &worker->flags);
	raw_spin_unlock(&acct->workers_lock);
	wake_up_new_task(tsk);
}

static bool io_wq_work_match_all(struct io_wq_work *work, void *data)
{
	return true;
}

static inline bool io_should_retry_thread(struct io_worker *worker, long err)
{
	/*
	 * Prevent perpetual task_work retry, if the task (or its group) is
	 * exiting.
	 */
	if (fatal_signal_pending(current))
		return false;
	if (worker->init_retries++ >= WORKER_INIT_LIMIT)
		return false;

	switch (err) {
	case -EAGAIN:
	case -ERESTARTSYS:
	case -ERESTARTNOINTR:
	case -ERESTARTNOHAND:
		return true;
	default:
		return false;
	}
}

static void queue_create_worker_retry(struct io_worker *worker)
{
	/*
	 * We only bother retrying because there's a chance that the
	 * failure to create a worker is due to some temporary condition
	 * in the forking task (e.g. outstanding signal); give the task
	 * some time to clear that condition.
	 */
	schedule_delayed_work(&worker->work,
			      msecs_to_jiffies(worker->init_retries * 5));
}

static void create_worker_cont(struct callback_head *cb)
{
	struct io_worker *worker;
	struct task_struct *tsk;
	struct io_wq *wq;
	struct io_wq_acct *acct;

	worker = container_of(cb, struct io_worker, create_work);
	clear_bit_unlock(0, &worker->create_state);
	wq = worker->wq;
	acct = io_wq_get_acct(worker);
	tsk = create_io_thread(io_wq_worker, worker, NUMA_NO_NODE);
	if (!IS_ERR(tsk)) {
		io_init_new_worker(wq, acct, worker, tsk);
		io_worker_release(worker);
		return;
	} else if (!io_should_retry_thread(worker, PTR_ERR(tsk))) {
		atomic_dec(&acct->nr_running);
		raw_spin_lock(&acct->workers_lock);
		acct->nr_workers--;
		if (!acct->nr_workers) {
			struct io_cb_cancel_data match = {
				.fn		= io_wq_work_match_all,
				.cancel_all	= true,
			};

			raw_spin_unlock(&acct->workers_lock);
			while (io_acct_cancel_pending_work(wq, acct, &match))
				;
		} else {
			raw_spin_unlock(&acct->workers_lock);
		}
		io_worker_ref_put(wq);
		kfree(worker);
		return;
	}

	/* re-create attempts grab a new worker ref, drop the existing one */
	io_worker_release(worker);
	queue_create_worker_retry(worker);
}

static void io_workqueue_create(struct work_struct *work)
{
	struct io_worker *worker = container_of(work, struct io_worker,
						work.work);
	struct io_wq_acct *acct = io_wq_get_acct(worker);

	if (!io_queue_worker_create(worker, acct, create_worker_cont))
		kfree(worker);
}

static bool create_io_worker(struct io_wq *wq, struct io_wq_acct *acct)
{
	struct io_worker *worker;
	struct task_struct *tsk;

	__set_current_state(TASK_RUNNING);

	worker = kzalloc(sizeof(*worker), GFP_KERNEL);
	if (!worker) {
fail:
		atomic_dec(&acct->nr_running);
		raw_spin_lock(&acct->workers_lock);
		acct->nr_workers--;
		raw_spin_unlock(&acct->workers_lock);
		io_worker_ref_put(wq);
		return false;
	}

	refcount_set(&worker->ref, 1);
	worker->wq = wq;
	worker->acct = acct;
	raw_spin_lock_init(&worker->lock);
	init_completion(&worker->ref_done);

	tsk = create_io_thread(io_wq_worker, worker, NUMA_NO_NODE);
	if (!IS_ERR(tsk)) {
		io_init_new_worker(wq, acct, worker, tsk);
	} else if (!io_should_retry_thread(worker, PTR_ERR(tsk))) {
		kfree(worker);
		goto fail;
	} else {
		INIT_DELAYED_WORK(&worker->work, io_workqueue_create);
		queue_create_worker_retry(worker);
	}

	return true;
}

/*
 * Iterate the passed in list and call the specific function for each
 * worker that isn't exiting
 */
static bool io_acct_for_each_worker(struct io_wq_acct *acct,
				    bool (*func)(struct io_worker *, void *),
				    void *data)
{
	struct io_worker *worker;
	bool ret = false;

	list_for_each_entry_rcu(worker, &acct->all_list, all_list) {
		if (io_worker_get(worker)) {
			/* no task if node is/was offline */
			if (worker->task)
				ret = func(worker, data);
			io_worker_release(worker);
			if (ret)
				break;
		}
	}

	return ret;
}

static bool io_wq_for_each_worker(struct io_wq *wq,
				  bool (*func)(struct io_worker *, void *),
				  void *data)
{
	for (int i = 0; i < IO_WQ_ACCT_NR; i++) {
		if (!io_acct_for_each_worker(&wq->acct[i], func, data))
			return false;
	}

	return true;
}

static bool io_wq_worker_wake(struct io_worker *worker, void *data)
{
	__set_notify_signal(worker->task);
	wake_up_process(worker->task);
	return false;
}

static void io_run_cancel(struct io_wq_work *work, struct io_wq *wq)
{
	do {
		atomic_or(IO_WQ_WORK_CANCEL, &work->flags);
		io_wq_submit_work(work);
		work = io_wq_free_work(work);
	} while (work);
}

static void io_wq_insert_work(struct io_wq *wq, struct io_wq_acct *acct,
			      struct io_wq_work *work, unsigned int work_flags)
{
	unsigned int hash;
	struct io_wq_work *tail;

	if (!__io_wq_is_hashed(work_flags)) {
append:
		wq_list_add_tail(&work->list, &acct->work_list);
		return;
	}

	hash = __io_get_work_hash(work_flags);
	tail = wq->hash_tail[hash];
	wq->hash_tail[hash] = work;
	if (!tail)
		goto append;

	wq_list_add_after(&work->list, &tail->list, &acct->work_list);
}

static bool io_wq_work_match_item(struct io_wq_work *work, void *data)
{
	return work == data;
}

void io_wq_enqueue(struct io_wq *wq, struct io_wq_work *work)
{
	unsigned int work_flags = atomic_read(&work->flags);
	struct io_wq_acct *acct = io_work_get_acct(wq, work_flags);
	struct io_cb_cancel_data match = {
		.fn		= io_wq_work_match_item,
		.data		= work,
		.cancel_all	= false,
	};
	bool do_create;

	/*
	 * If io-wq is exiting for this task, or if the request has explicitly
	 * been marked as one that should not get executed, cancel it here.
	 */
	if (test_bit(IO_WQ_BIT_EXIT, &wq->state) ||
	    (work_flags & IO_WQ_WORK_CANCEL)) {
		io_run_cancel(work, wq);
		return;
	}

	raw_spin_lock(&acct->lock);
	io_wq_insert_work(wq, acct, work, work_flags);
	clear_bit(IO_ACCT_STALLED_BIT, &acct->flags);
	raw_spin_unlock(&acct->lock);

	rcu_read_lock();
	do_create = !io_acct_activate_free_worker(acct);
	rcu_read_unlock();

	if (do_create && ((work_flags & IO_WQ_WORK_CONCURRENT) ||
	    !atomic_read(&acct->nr_running))) {
		bool did_create;

		did_create = io_wq_create_worker(wq, acct);
		if (likely(did_create))
			return;

		raw_spin_lock(&acct->workers_lock);
		if (acct->nr_workers) {
			raw_spin_unlock(&acct->workers_lock);
			return;
		}
		raw_spin_unlock(&acct->workers_lock);

		/* fatal condition, failed to create the first worker */
		io_acct_cancel_pending_work(wq, acct, &match);
	}
}

/*
 * Work items that hash to the same value will not be done in parallel.
 * Used to limit concurrent writes, generally hashed by inode.
 */
void io_wq_hash_work(struct io_wq_work *work, void *val)
{
	unsigned int bit;

	bit = hash_ptr(val, IO_WQ_HASH_ORDER);
	atomic_or(IO_WQ_WORK_HASHED | (bit << IO_WQ_HASH_SHIFT), &work->flags);
}

static bool __io_wq_worker_cancel(struct io_worker *worker,
				  struct io_cb_cancel_data *match,
				  struct io_wq_work *work)
{
	if (work && match->fn(work, match->data)) {
		atomic_or(IO_WQ_WORK_CANCEL, &work->flags);
		__set_notify_signal(worker->task);
		return true;
	}

	return false;
}

static bool io_wq_worker_cancel(struct io_worker *worker, void *data)
{
	struct io_cb_cancel_data *match = data;

	/*
	 * Hold the lock to avoid ->cur_work going out of scope, caller
	 * may dereference the passed in work.
	 */
	raw_spin_lock(&worker->lock);
	if (__io_wq_worker_cancel(worker, match, worker->cur_work))
		match->nr_running++;
	raw_spin_unlock(&worker->lock);

	return match->nr_running && !match->cancel_all;
}

static inline void io_wq_remove_pending(struct io_wq *wq,
					struct io_wq_acct *acct,
					 struct io_wq_work *work,
					 struct io_wq_work_node *prev)
{
	unsigned int hash = io_get_work_hash(work);
	struct io_wq_work *prev_work = NULL;

	if (io_wq_is_hashed(work) && work == wq->hash_tail[hash]) {
		if (prev)
			prev_work = container_of(prev, struct io_wq_work, list);
		if (prev_work && io_get_work_hash(prev_work) == hash)
			wq->hash_tail[hash] = prev_work;
		else
			wq->hash_tail[hash] = NULL;
	}
	wq_list_del(&acct->work_list, &work->list, prev);
}

static bool io_acct_cancel_pending_work(struct io_wq *wq,
					struct io_wq_acct *acct,
					struct io_cb_cancel_data *match)
{
	struct io_wq_work_node *node, *prev;
	struct io_wq_work *work;

	raw_spin_lock(&acct->lock);
	wq_list_for_each(node, prev, &acct->work_list) {
		work = container_of(node, struct io_wq_work, list);
		if (!match->fn(work, match->data))
			continue;
		io_wq_remove_pending(wq, acct, work, prev);
		raw_spin_unlock(&acct->lock);
		io_run_cancel(work, wq);
		match->nr_pending++;
		/* not safe to continue after unlock */
		return true;
	}
	raw_spin_unlock(&acct->lock);

	return false;
}

static void io_wq_cancel_pending_work(struct io_wq *wq,
				      struct io_cb_cancel_data *match)
{
	int i;
retry:
	for (i = 0; i < IO_WQ_ACCT_NR; i++) {
		struct io_wq_acct *acct = io_get_acct(wq, i == 0);

		if (io_acct_cancel_pending_work(wq, acct, match)) {
			if (match->cancel_all)
				goto retry;
			break;
		}
	}
}

static void io_acct_cancel_running_work(struct io_wq_acct *acct,
					struct io_cb_cancel_data *match)
{
	raw_spin_lock(&acct->workers_lock);
	io_acct_for_each_worker(acct, io_wq_worker_cancel, match);
	raw_spin_unlock(&acct->workers_lock);
}

static void io_wq_cancel_running_work(struct io_wq *wq,
				       struct io_cb_cancel_data *match)
{
	rcu_read_lock();

	for (int i = 0; i < IO_WQ_ACCT_NR; i++)
		io_acct_cancel_running_work(&wq->acct[i], match);

	rcu_read_unlock();
}

enum io_wq_cancel io_wq_cancel_cb(struct io_wq *wq, work_cancel_fn *cancel,
				  void *data, bool cancel_all)
{
	struct io_cb_cancel_data match = {
		.fn		= cancel,
		.data		= data,
		.cancel_all	= cancel_all,
	};

	/*
	 * First check pending list, if we're lucky we can just remove it
	 * from there. CANCEL_OK means that the work is returned as-new,
	 * no completion will be posted for it.
	 *
	 * Then check if a free (going busy) or busy worker has the work
	 * currently running. If we find it there, we'll return CANCEL_RUNNING
	 * as an indication that we attempt to signal cancellation. The
	 * completion will run normally in this case.
	 *
	 * Do both of these while holding the acct->workers_lock, to ensure that
	 * we'll find a work item regardless of state.
	 */
	io_wq_cancel_pending_work(wq, &match);
	if (match.nr_pending && !match.cancel_all)
		return IO_WQ_CANCEL_OK;

	io_wq_cancel_running_work(wq, &match);
	if (match.nr_running && !match.cancel_all)
		return IO_WQ_CANCEL_RUNNING;

	if (match.nr_running)
		return IO_WQ_CANCEL_RUNNING;
	if (match.nr_pending)
		return IO_WQ_CANCEL_OK;
	return IO_WQ_CANCEL_NOTFOUND;
}

static int io_wq_hash_wake(struct wait_queue_entry *wait, unsigned mode,
			    int sync, void *key)
{
	struct io_wq *wq = container_of(wait, struct io_wq, wait);
	int i;

	list_del_init(&wait->entry);

	rcu_read_lock();
	for (i = 0; i < IO_WQ_ACCT_NR; i++) {
		struct io_wq_acct *acct = &wq->acct[i];

		if (test_and_clear_bit(IO_ACCT_STALLED_BIT, &acct->flags))
			io_acct_activate_free_worker(acct);
	}
	rcu_read_unlock();
	return 1;
}

struct io_wq *io_wq_create(unsigned bounded, struct io_wq_data *data)
{
	int ret, i;
	struct io_wq *wq;

	if (WARN_ON_ONCE(!bounded))
		return ERR_PTR(-EINVAL);

	wq = kzalloc(sizeof(struct io_wq), GFP_KERNEL);
	if (!wq)
		return ERR_PTR(-ENOMEM);

	refcount_inc(&data->hash->refs);
	wq->hash = data->hash;

	ret = -ENOMEM;

	if (!alloc_cpumask_var(&wq->cpu_mask, GFP_KERNEL))
		goto err;
	cpuset_cpus_allowed(data->task, wq->cpu_mask);
	wq->acct[IO_WQ_ACCT_BOUND].max_workers = bounded;
	wq->acct[IO_WQ_ACCT_UNBOUND].max_workers =
				task_rlimit(current, RLIMIT_NPROC);
	INIT_LIST_HEAD(&wq->wait.entry);
	wq->wait.func = io_wq_hash_wake;
	for (i = 0; i < IO_WQ_ACCT_NR; i++) {
		struct io_wq_acct *acct = &wq->acct[i];

		atomic_set(&acct->nr_running, 0);

		raw_spin_lock_init(&acct->workers_lock);
		INIT_HLIST_NULLS_HEAD(&acct->free_list, 0);
		INIT_LIST_HEAD(&acct->all_list);

		INIT_WQ_LIST(&acct->work_list);
		raw_spin_lock_init(&acct->lock);
	}

	wq->task = get_task_struct(data->task);
	atomic_set(&wq->worker_refs, 1);
	init_completion(&wq->worker_done);
	ret = cpuhp_state_add_instance_nocalls(io_wq_online, &wq->cpuhp_node);
	if (ret) {
		put_task_struct(wq->task);
		goto err;
	}

	return wq;
err:
	io_wq_put_hash(data->hash);
	free_cpumask_var(wq->cpu_mask);
	kfree(wq);
	return ERR_PTR(ret);
}

static bool io_task_work_match(struct callback_head *cb, void *data)
{
	struct io_worker *worker;

	if (cb->func != create_worker_cb && cb->func != create_worker_cont)
		return false;
	worker = container_of(cb, struct io_worker, create_work);
	return worker->wq == data;
}

void io_wq_exit_start(struct io_wq *wq)
{
	set_bit(IO_WQ_BIT_EXIT, &wq->state);
}

static void io_wq_cancel_tw_create(struct io_wq *wq)
{
	struct callback_head *cb;

	while ((cb = task_work_cancel_match(wq->task, io_task_work_match, wq)) != NULL) {
		struct io_worker *worker;

		worker = container_of(cb, struct io_worker, create_work);
		io_worker_cancel_cb(worker);
		/*
		 * Only the worker continuation helper has worker allocated and
		 * hence needs freeing.
		 */
		if (cb->func == create_worker_cont)
			kfree(worker);
	}
}

static void io_wq_exit_workers(struct io_wq *wq)
{
	if (!wq->task)
		return;

	io_wq_cancel_tw_create(wq);

	rcu_read_lock();
	io_wq_for_each_worker(wq, io_wq_worker_wake, NULL);
	rcu_read_unlock();
	io_worker_ref_put(wq);
	wait_for_completion(&wq->worker_done);

	spin_lock_irq(&wq->hash->wait.lock);
	list_del_init(&wq->wait.entry);
	spin_unlock_irq(&wq->hash->wait.lock);

	put_task_struct(wq->task);
	wq->task = NULL;
}

static void io_wq_destroy(struct io_wq *wq)
{
	struct io_cb_cancel_data match = {
		.fn		= io_wq_work_match_all,
		.cancel_all	= true,
	};

	cpuhp_state_remove_instance_nocalls(io_wq_online, &wq->cpuhp_node);
	io_wq_cancel_pending_work(wq, &match);
	free_cpumask_var(wq->cpu_mask);
	io_wq_put_hash(wq->hash);
	kfree(wq);
}

void io_wq_put_and_exit(struct io_wq *wq)
{
	WARN_ON_ONCE(!test_bit(IO_WQ_BIT_EXIT, &wq->state));

	io_wq_exit_workers(wq);
	io_wq_destroy(wq);
}

struct online_data {
	unsigned int cpu;
	bool online;
};

static bool io_wq_worker_affinity(struct io_worker *worker, void *data)
{
	struct online_data *od = data;

	if (od->online)
		cpumask_set_cpu(od->cpu, worker->wq->cpu_mask);
	else
		cpumask_clear_cpu(od->cpu, worker->wq->cpu_mask);
	return false;
}

static int __io_wq_cpu_online(struct io_wq *wq, unsigned int cpu, bool online)
{
	struct online_data od = {
		.cpu = cpu,
		.online = online
	};

	rcu_read_lock();
	io_wq_for_each_worker(wq, io_wq_worker_affinity, &od);
	rcu_read_unlock();
	return 0;
}

static int io_wq_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct io_wq *wq = hlist_entry_safe(node, struct io_wq, cpuhp_node);

	return __io_wq_cpu_online(wq, cpu, true);
}

static int io_wq_cpu_offline(unsigned int cpu, struct hlist_node *node)
{
	struct io_wq *wq = hlist_entry_safe(node, struct io_wq, cpuhp_node);

	return __io_wq_cpu_online(wq, cpu, false);
}

int io_wq_cpu_affinity(struct io_uring_task *tctx, cpumask_var_t mask)
{
	cpumask_var_t allowed_mask;
	int ret = 0;

	if (!tctx || !tctx->io_wq)
		return -EINVAL;

	if (!alloc_cpumask_var(&allowed_mask, GFP_KERNEL))
		return -ENOMEM;

	rcu_read_lock();
	cpuset_cpus_allowed(tctx->io_wq->task, allowed_mask);
	if (mask) {
		if (cpumask_subset(mask, allowed_mask))
			cpumask_copy(tctx->io_wq->cpu_mask, mask);
		else
			ret = -EINVAL;
	} else {
		cpumask_copy(tctx->io_wq->cpu_mask, allowed_mask);
	}
	rcu_read_unlock();

	free_cpumask_var(allowed_mask);
	return ret;
}

/*
 * Set max number of unbounded workers, returns old value. If new_count is 0,
 * then just return the old value.
 */
int io_wq_max_workers(struct io_wq *wq, int *new_count)
{
	struct io_wq_acct *acct;
	int prev[IO_WQ_ACCT_NR];
	int i;

	BUILD_BUG_ON((int) IO_WQ_ACCT_BOUND   != (int) IO_WQ_BOUND);
	BUILD_BUG_ON((int) IO_WQ_ACCT_UNBOUND != (int) IO_WQ_UNBOUND);
	BUILD_BUG_ON((int) IO_WQ_ACCT_NR      != 2);

	for (i = 0; i < IO_WQ_ACCT_NR; i++) {
		if (new_count[i] > task_rlimit(current, RLIMIT_NPROC))
			new_count[i] = task_rlimit(current, RLIMIT_NPROC);
	}

	for (i = 0; i < IO_WQ_ACCT_NR; i++)
		prev[i] = 0;

	rcu_read_lock();

	for (i = 0; i < IO_WQ_ACCT_NR; i++) {
		acct = &wq->acct[i];
		raw_spin_lock(&acct->workers_lock);
		prev[i] = max_t(int, acct->max_workers, prev[i]);
		if (new_count[i])
			acct->max_workers = new_count[i];
		raw_spin_unlock(&acct->workers_lock);
	}
	rcu_read_unlock();

	for (i = 0; i < IO_WQ_ACCT_NR; i++)
		new_count[i] = prev[i];

	return 0;
}

static __init int io_wq_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "io-wq/online",
					io_wq_cpu_online, io_wq_cpu_offline);
	if (ret < 0)
		return ret;
	io_wq_online = ret;
	return 0;
}
subsys_initcall(io_wq_init);
