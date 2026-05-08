#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

// 잠든 스레드들을 깨울 시각 순서대로 저장하는 리스트
static struct list sleep_list;

// wakeup_tick 기준으로 정렬하기 위한 비교 함수
static bool
wakeup_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	// TODO: wakeup_tick이 더 작은 스레드가 앞에 오도록 비교
	// elem을 가지고 있는 struct thread 비교
	struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);

    return ta->wakeup_tick < tb->wakeup_tick;
}
static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);
static void timer_sleeplist(int64_t ticks);
static bool sort_wakeup_tick(const struct list_elem *a, const struct list_elem *b, void *aux);

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void timer_init(void)
{
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	/* sleep_list 초기화 */
	list_init(&sleep_list);

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	// 타이머 인터럽트가 동작하기 전에 sleep list 초기화
	list_init(&sleep_list);
	
	intr_register_ext (0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void timer_calibrate(void)
{
	unsigned high_bit, test_bit;

	ASSERT(intr_get_level() == INTR_ON);
	printf("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops(loops_per_tick << 1))
	{
		loops_per_tick <<= 1;
		ASSERT(loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops(high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf("%'" PRIu64 " loops/s.\n", (uint64_t)loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks(void)
{
	enum intr_level old_level = intr_disable();
	int64_t t = ticks;
	intr_set_level(old_level);
	barrier();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed(int64_t then)
{
	return timer_ticks() - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
	enum intr_level old_level;
	struct thread *cur;

	ASSERT (intr_get_level () == INTR_ON);

	// 0 이하의 tick은 바로 반환
	if (ticks <= 0) {
		return;
	}
	
	// sleep list 삽입과 block을 원자적으로 처리하기 위해 인터럽트 비활성화 */
	old_level = intr_disable ();
	cur = thread_current ();

	// 현재 스레드가 깨어날 tick 기록
	cur->wakeup_tick = timer_ticks () + ticks;

	// TODO: wakeup_tick 기준으로 sleep_list에 정렬 삽입
	list_insert_ordered (&sleep_list, &cur->elem, wakeup_less, NULL);
	
	// TODO: 현재 스레드를 block 상태로 전환
	thread_block();

	intr_set_level (old_level);
}

/* Suspends execution for approximately MS milliseconds. */
void timer_msleep(int64_t ms)
{
	real_time_sleep(ms, 1000);
}

/* Suspends execution for approximately US                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               microseconds. */
void timer_usleep(int64_t us)
{
	real_time_sleep(us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void timer_nsleep(int64_t ns)
{
	real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void timer_print_stats(void)
{
	printf("Timer: %" PRId64 " ticks\n", timer_ticks());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	bool should_yield = false;

	ticks++;

	// TODO: 깨어날 시간이 된 sleeping thread를 순서대로 깨우기
	while (!list_empty (&sleep_list)) {
        struct thread *t = list_entry (list_front (&sleep_list), struct thread, elem);

        if (t->wakeup_tick > ticks) {
            break;
		}

        list_pop_front (&sleep_list);
        thread_unblock (t);

		/* sleep이 끝나 깨어난 스레드 중 현재보다 우선순위가 높은 스레드가 있으면,
		인터럽트가 끝난 뒤 바로 그 스레드가 실행될 수 있도록 표시 */
		if (t->priority > thread_current ()->priority) {
			should_yield = true;
		}
    }
	/* 타이머 인터럽트 안에서는 바로 양보하지 않고,
	인터럽트 복귀 시점에 스케줄링되게 한다. */
	if (should_yield) {
		intr_yield_on_return ();
	}

	thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops(unsigned loops)
{
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait(loops);

	/* If the tick count changed, we iterated too long. */
	barrier();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait(int64_t loops)
{
	while (loops-- > 0)
		barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep(int64_t num, int32_t denom)
{
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT(intr_get_level() == INTR_ON);
	if (ticks > 0)
	{
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep(ticks);
	}
	else
	{
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT(denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}