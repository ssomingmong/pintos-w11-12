/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"


/* thread waiters를 높은 priority 순으로 유지하기 위한 비교 함수. */
static bool cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);
static bool cmp_sema_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

/* 두 thread waiter를 priority로 비교한다. */
static bool
cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);

	return ta->priority > tb -> priority;
}
/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */

void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		/* semaphore waiters도 높은 priority가 앞에 오게 정렬한다. */
		list_insert_ordered (&sema->waiters, &thread_current()->elem, cmp_priority, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}
	
/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

static bool
sema_priority_more (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *ta = list_entry(a, struct thread, elem);
	struct thread *tb = list_entry(b, struct thread, elem);
	
	return ta->priority > tb->priority;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	struct thread *woken = NULL;
	bool should_yield = false;

	ASSERT (sema != NULL);
	old_level = intr_disable ();
	if (!list_empty (&sema->waiters))
	{
		// waiter들 중 가장 높은 우선순위가 스레드가 먼저 깨어나도록 정렬
		list_sort (&sema->waiters, sema_priority_more, NULL);

		// 가장 높은 우선순위 waiter를 꺼내 READY 상태로 바꾼다.
		woken = list_entry (list_pop_front (&sema->waiters), struct thread, elem);
		thread_unblock (woken);

		/* 세마포어를 기다리던 스레드를 깨운 뒤,
		그 스레드가 현재보다 우선순위가 높으면 CPU를 넘겨야 함 */
		if (woken->priority > thread_current ()->priority) 
		{
			/* 인터럽트 컨텍스트에서는 즉시 양보하지 않고,
			인터럽트가 끝난 뒤 스케줄링이 일어나도록 예약 */
			if (intr_context ()) {
				intr_yield_on_return ();
			}
			else {
				should_yield = true;
			}
		}
	}
	sema->value++;
	intr_set_level (old_level);

	// 일반 실행 흐름에서는 여기서 직접 CPU를 양보
	if (should_yield) {
		thread_yield ();
	}
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
/* lock을 잡기 전에 holder가 이미 존재하면,
현재 스레드는 이 lock 때문에 기다리게 된다.
따라서 현재 스레드를 donor로 등록하고,
holder 쪽으로 priority donation을 시작. */
void
lock_acquire (struct lock *lock) {
	
	// lock 포인터가 진짜 있어야 한다.
	ASSERT (lock != NULL);
	// 인터럽트 핸들러 문맥에서는 호출하면 안 된다.
	ASSERT (!intr_context ());
	// 현재 스레드가 이미 이 lock을 들고 있으면 안 된다. 
	ASSERT (!lock_held_by_current_thread (lock));

	if (lock->holder != NULL)
	{
		struct thread *curr = thread_current ();

		// nested donation 추적을 위해 현재 기다리는 lock 기록
		curr->wait_on_lock = lock;

		// 현재 스레드를 holder의 donor 목록에 추가
		list_push_back (&lock->holder->donations, &curr->donation_elem);

		// 높은 우선순위를 holder에게 전파
		donate_priority ();
	}

	// donation을 반영한 뒤 실제로 lock이 풀릴 때까지 기다린다.
	sema_down (&lock->semaphore);

	// 이제 lock을 얻었으므로 더 이상 기다리는 상태가 아님
	thread_current ()->wait_on_lock = NULL;

	// 현재 스레드가 이 lock의 새 holder가 된다.
	lock->holder = thread_current ();
}





/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
/* lock을 놓을 때는 이 lock 때문에 들어온 donation만 제거하고,
남아 있는 donor들을 기준으로 현재 우선순위를 다시 계산해야 함. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	remove_with_lock (lock);
	refresh_priority ();
	lock->holder = NULL;
	sema_up (&lock->semaphore);
	thread_yield_if_not_highest ();
	return;

#if 0
	while (e != list_end(&current->donations)) {
    	struct list_elem *next = list_next(e);

    	struct thread *donor = list_entry(e, struct thread, donation_elem);

    	if (donor->wait_on_lock == lock) {
        	list_remove(e);
		}

    	e = next;
	}

	current->priority = current->base_priority;

	if (!list_empty(&current->donations)) {
		struct list_elem *max_elem = list_max(&current->donations, donation_priority_sort, NULL);
		t = list_entry(max_elem, struct thread, donation_elem);
		if (current->priority < t->priority) {
			current->priority = t->priority;
		}
	}

	// 이제 이 lock의 holder는 없음
	lock->holder = NULL;

	// 기다리던 다음 스레드가 lock을 얻을 수 있도록 깨움
	sema_up (&lock->semaphore);

	if(!list_empty(&thread_current()->donations))
	{
	t = list_entry(list_begin(&thread_current()->donations), struct thread, donation_elem); 
		
	if (thread_current ()->priority < t->priority)
		{	
			thread_current ()->priority = t->priority;
		}
	}

	// if (!list_empty(&ready_list) && listlist_entry(list_begin(&ready_list), struct thread, elem)->priority)
	// {
	// 	thread_yield();
	// }
#endif
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
	int priority;                       /* cond waiter를 정렬할 때 쓸 priority */
};

/* cond->waiters 안의 semaphore_elem을 priority로 비교한다. */
static bool
cmp_sema_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

	return sa->priority > sb->priority;
}

/* condition waiters는 thread가 아니라 semaphore_elem을 저장하므로,
각 waiter 안에서 결국 어떤 우선순위 thread가 깨어나는지를 기준으로 비교 */
static bool
cond_priority_more (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

	struct thread *ta = list_entry (list_front (&sa->semaphore.waiters), struct thread, elem);
	struct thread *tb = list_entry (list_front (&sb->semaphore.waiters), struct thread, elem);

	return ta->priority > tb->priority;
}
/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	/* cond waiter도 생성 시점의 priority를 저장해 정렬 기준으로 쓴다. */
	waiter.priority = thread_current()->priority;
	/* cond waiters를 높은 priority 순으로 유지한다. */
	list_insert_ordered (&cond->waiters, &waiter.elem, cmp_sema_priority, NULL);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
bool
max_priority(const struct list_elem *a, const struct list_elem *b, void *aux) {
	struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

	struct semaphore *sema1 = &(sa->semaphore);
	struct semaphore *sema2 = &(sb->semaphore);

	struct thread *ta = list_entry(list_front(&sema1->waiters), struct thread, elem);
	struct thread *tb = list_entry(list_front(&sema2->waiters), struct thread, elem);

	if (ta->priority < tb->priority) {
		return true;
	}
	return false;
} 

void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
	{
		// condition waiters 중 우선순위가 가장 높은 waiter를 먼저 깨우도록 정렬
		list_sort (&cond->waiters, cond_priority_more, NULL);

		// 선택된 waiter의 세마포어를 개워서, 그 waiter와 연결된 스레가 READY 상태가 되게 한다.
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
