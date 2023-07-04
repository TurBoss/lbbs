/*
 * LBBS -- The Lightweight Bulletin Board System
 *
 * Copyright (C) 2023, Naveen Albert
 *
 * Naveen Albert <bbs@phreaknet.org>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Thread management
 *
 * \author Naveen Albert <bbs@phreaknet.org>
 */

#define BBS_PTHREAD_WRAPPER_FILE

#include "include/bbs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <signal.h> /* use pthread_kill */

#include "include/utils.h"
#include "include/linkedlists.h"

static __thread int my_tid = 0;

int bbs_gettid(void)
{
	int tid;

	/* If we've called this before, return the cached value to avoid a system call.
	 * Since every call to __bbs_log calls bbs_gettid, this is an important optimization. */
	if (my_tid) {
		return my_tid;
	}

	/* We cannot merely have a macro defining gettid if it's not defined,
	 * because the native return types of gettid and SYS_gettid differ.
	 * Here, we implicitly cast to int before returning.
	 */
#ifdef gettid
	/* gettid() is not very portable at all.
	 * It works on Debian 11 but not on Debian 10 for me. */
	tid = gettid();
#else
	tid = (int) syscall(SYS_gettid);
#endif
	my_tid = tid; /* Save the value for future reference, so we don't need to do this again. */
	return tid;
}

struct thread_list_t {
	RWLIST_ENTRY(thread_list_t) list;
	char *name;
	pthread_t id;
	int lwp;
	int start;
	int end;
	unsigned int detached:1;
	unsigned int waitingjoin:1;
};

static RWLIST_HEAD_STATIC(thread_list, thread_list_t);

static void thread_register(char *name, int detached)
{
	struct thread_list_t *new = calloc(1, sizeof(*new));

	if (ALLOC_FAILURE(new)) {
		return;
	}

	new->start = (int) time(NULL);
	new->id = pthread_self();
	new->lwp = bbs_gettid();
	new->name = name; /* steal the allocated memory for the thread name */
	SET_BITFIELD(new->detached, detached);
	RWLIST_WRLOCK(&thread_list);
	RWLIST_INSERT_TAIL(&thread_list, new, list);
	RWLIST_UNLOCK(&thread_list);
	bbs_debug(3, "Thread %d spawned from %s\n", new->lwp, new->name);
}

static int __thread_unregister(pthread_t id, const char *file, int line, const char *func)
{
	struct thread_list_t *x;
	int remove = 0;
	int lwp = -1;

	RWLIST_WRLOCK(&thread_list);
	RWLIST_TRAVERSE_SAFE_BEGIN(&thread_list, x, list) {
		if (x->id == id) {
			if (x->detached || x->waitingjoin) {
				RWLIST_REMOVE_CURRENT(list);
				remove = 1;
			} else {
				x->waitingjoin = 1;
				x->end = (int) time(NULL);
			}
			lwp = x->lwp;
			break;
		}
	}
	RWLIST_TRAVERSE_SAFE_END;
	if (x) {
		if (remove) {
			if (x->detached) {
				bbs_debug(3, "Thread %d is exiting (detached)\n", x->lwp);
			} else {
				bbs_debug(3, "Thread %d has been joined by thread %d at %s:%d %s()\n", x->lwp, bbs_gettid(), file, line, func);
			}
			free_if(x->name);
			free(x);
		} else {
			bbs_debug(3, "Thread %d is exiting (%s)\n", lwp, "must be joined");
		}
	} else {
		bbs_error("Thread %lu not found?\n", id);
	}
	/* On shutdown, bbs_thread_cleanup will occur right after all modules have unloaded, i.e. all child threads have exited.
	 * If we release the lock right after the traversal ends, bbs_thread_cleanup will find the list empty and return, and
	 * then bbs_log_close is called not long afterwards, i.e. the logger could proceed to shut down before we finish logging above. */
	RWLIST_UNLOCK(&thread_list);
	return lwp;
}

static const char *thread_state_name(struct thread_list_t *cur)
{
	return cur->detached ? "detached" : cur->waitingjoin ? "waitjoin" : "joinable";
}

void bbs_thread_cleanup(void)
{
	char elapsed[24];
	struct thread_list_t *x;
	int now = (int) time(NULL);

	/* All spawned threads should have exited by now. Let's see if that's the case. */
	RWLIST_WRLOCK(&thread_list);
	while ((x = RWLIST_REMOVE_HEAD(&thread_list, list))) {
		/* In theory, all registered threads should have exited / been joined by this phase of shutdown.
		 * If not, then it's probably a bug, a thread we forgot to exit, join, etc.
		 * especially if it's a thread that has been in the waitjoin state for some time (more than a couple seconds).
		 * Be nice and free the memory anyways. */
		print_time_elapsed(x->waitingjoin ? x->end : x->start, now, elapsed, sizeof(elapsed));
		bbs_warning("Thread still registered at shutdown: %d (%s %s) %s\n", x->lwp, thread_state_name(x), elapsed, x->name);
		free_if(x->name);
		free(x);
	}
	RWLIST_UNLOCK(&thread_list);
}

static void thread_unregister(void *id)
{
	pthread_t *thread = id;
	__thread_unregister(*thread, NULL, 0, NULL);
}

int bbs_pthread_tid(pthread_t thread)
{
	struct thread_list_t *x;
	int lwp = -1;

	RWLIST_RDLOCK(&thread_list);
	RWLIST_TRAVERSE(&thread_list, x, list) {
		if (thread == x->id) {
			lwp = x->lwp;
			break;
		}
	}
	RWLIST_UNLOCK(&thread_list);

	return lwp;
}

int bbs_dump_threads(int fd)
{
	char elapsed[24];
	int threads = 0;
	struct thread_list_t *cur;
	int now = (int) time(NULL);

	bbs_dprintf(fd, "%3d %6d (%s)\n", 0, getpid(), "PID / main thread");
	RWLIST_RDLOCK(&thread_list);
	RWLIST_TRAVERSE(&thread_list, cur, list) {
		threads++;
		print_time_elapsed(cur->waitingjoin ? cur->end : cur->start, now, elapsed, sizeof(elapsed));
		bbs_dprintf(fd, "%3d %6d (%9lu) [%12p] (%s %10s) %s\n", threads, cur->lwp, cur->id, (void *) cur->id, thread_state_name(cur), elapsed, cur->name);
	}
	RWLIST_UNLOCK(&thread_list);
	bbs_dprintf(fd, "%d active threads registered (may be incomplete).\n", threads);
	return 0;
}

void bbs_pthread_disable_cancel(void)
{
	int oldstate;
	/* In LINUX, it's okay to pass NULL for the 2nd argument, but this is not portable. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
}

void bbs_pthread_enable_cancel(void)
{
	int oldstate;
	/* In LINUX, it's okay to pass NULL for the 2nd argument, but this is not portable. */
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
}

int bbs_pthread_cancel_kill(pthread_t thread)
{
	int res;

	res = pthread_cancel(thread);
	if (res) {
		if (res == ESRCH) {
			bbs_debug(3, "Thread %lu no longer exists\n", thread);
		} else {
			bbs_warning("Could not cancel thread %lu: %s\n", thread, strerror(res));
		}
	}

	res = pthread_kill(thread, SIGURG);
	if (res) {
		if (res == ESRCH) {
			bbs_debug(3, "Thread %lu no longer exists\n", thread);
		} else {
			bbs_warning("Could not kill thread %lu: %s\n", thread, strerror(res));
		}
	} else {
		bbs_debug(3, "Killed thread %lu\n", thread);
	}
	return res;
}

int __bbs_pthread_join(pthread_t thread, void **retval, const char *file, const char *func, int line)
{
	void *tmp;
	int res;
	struct thread_list_t *x;
	int lwp;
	int waiting_join;

	RWLIST_RDLOCK(&thread_list);
	RWLIST_TRAVERSE(&thread_list, x, list) {
		if (thread == x->id) {
			lwp = x->lwp;
			if (x->detached) {
				bbs_error("Can't join detached LWP %d at %s:%d %s()\n", lwp, file, line, func);
				lwp = 0;
			}
			waiting_join = x->waitingjoin;
			break;
		}
	}
	RWLIST_UNLOCK(&thread_list);

	if (!x) {
		bbs_error("Thread %lu not registered\n", thread);
		return -1;
	} else if (!lwp) {
		return -1;
	}

	bbs_debug(6, "Attempting to join thread %lu (LWP %d) at %s:%d %s()\n", thread, lwp, file, line, func);

	if (!waiting_join) {
		struct timespec ts;
		/* This is suspicious... we may end up hanging if the thread doesn't exit imminently */
		/* Don't immediately emit a warning, because the thread may be just about to exit
		 * and thus wasn't waitingjoin when we checked. This prevents superflous warnings,
		 * by waiting to join for a brief moment and only warning if the thread doesn't join in that time. */
		ts.tv_sec = 0;
		ts.tv_nsec = 30000000; /* 30 ms */
		res = pthread_timedjoin_np(thread, retval ? retval : &tmp, &ts); /* This is not POSIX portable */
		if (res && res == ETIMEDOUT) {
			/* The thread hasn't exited yet. At this point, it's more likely that something is actually wrong. */
			bbs_warning("Thread %d is not currently waiting to be joined\n", lwp);
			/* Now, proceed as normal and do a ~blocking pthread_join */
			/* Seems that after using pthread_timedjoin_np, you can't do a blocking pthread_join anymore. So loop */
			while (res && res == ETIMEDOUT) {
				res = pthread_timedjoin_np(thread, retval ? retval : &tmp, &ts);
			}
		}
	} else {
		res = pthread_join(thread, retval ? retval : &tmp);
	}

	if (res) {
		bbs_error("pthread_join(%lu) at %s:%d %s(): %s\n", thread, file, line, func, strerror(res));
		return res;
	}
	res = __thread_unregister(thread, file, line, func);
	if (res == -1) {
		bbs_error("Thread %d attempted to join nonjoinable thread %lu at %s:%d %s()\n", bbs_gettid(), thread, file, line, func);
		return -1; /* pthread_join may have returned 0, but if the thread was detached (though it can't be here!), we probably can't trust its return value */
	}
	return 0;
}

/*!
 * \brief support for thread inventory. The start routine is wrapped by
 * thread_run(), so that thread_register() and
 * thread_unregister() know the thread identifier.
 */
struct thr_arg {
	void *(*start_routine)(void *);
	void *data;
	char *name;
	unsigned int detached:1;
};

static void *thread_run(void *data)
{
	void *ret;
	struct thr_arg a = *((struct thr_arg *) data);	/* make a local copy */

	/* Note that even though data->name is a pointer to allocated memory,
	 * we are not freeing it here because thread_register is going to
	 * keep a copy of the pointer and then thread_unregister will
	 * free the memory */
	free(data);
	thread_register(a.name, a.detached);
	pthread_cleanup_push(thread_unregister, (void *) pthread_self());

	ret = a.start_routine(a.data);

	pthread_cleanup_pop(1);
	return ret;
}

static int create_thread(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, int detached, const char *file, const char *func, int line, const char *start_fn)
{
	int res;
	struct thr_arg *a;

	a = malloc(sizeof(*a));
	if (ALLOC_FAILURE(a)) {
		/* If we can't malloc, what makes us think thread creation will succeed?
		 * Just abort now. */
		return -1;
	}

	/* Start thread execution at thread_run so we can push the cleanup function */
	a->start_routine = start_routine;
	a->data = data;
	a->detached = detached ? 1 : 0;
	start_routine = thread_run;
	res = asprintf(&a->name, "%-21s started by thread %d at %s:%d %s()", start_fn, bbs_gettid(), file, line, func);
	if (unlikely(res < 0)) {
		free(a);
		return -1;
	}
	data = a;

	res = pthread_create(thread, attr, start_routine, data);
	if (unlikely(res)) {
		bbs_error("Failed to spawn thread to execute %s(): %s\n", start_fn, strerror(errno));
		/* The thread never spawned, so cleanup the mess we made */
		free_if(a->name); /* We continued on failure, so this could be NULL. */
		free(a);
	}
	return res;
}

static int __bbs_pthread_create_detached_full(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, const char *file, const char *func, int line, const char *start_fn, int detached)
{
	int res;
	pthread_attr_t attrlocal;
	pthread_attr_t *attrptr = attr ? attr : &attrlocal;

	if (!attr) {
		pthread_attr_init(&attrlocal);
	}
	res = pthread_attr_setdetachstate(attrptr, PTHREAD_CREATE_DETACHED);
	if (unlikely(res)) {
		bbs_error("pthread_attr_setdetachstate: %s\n", strerror(res));
		return -1;
	}
	return create_thread(thread, attrptr, start_routine, data, detached, file, func, line, start_fn);
}

int __bbs_pthread_create_detached(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, const char *file, const char *func, int line, const char *start_fn)
{
	return __bbs_pthread_create_detached_full(thread, attr, start_routine, data, file, func, line, start_fn, 1);
}

int __bbs_pthread_create(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, const char *file, const char *func, int line, const char *start_fn)
{
	return create_thread(thread, attr, start_routine, data, 0, file, func, line, start_fn);
}
