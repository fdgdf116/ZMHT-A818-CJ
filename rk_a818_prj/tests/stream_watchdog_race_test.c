/* Deterministically inject a DMA start immediately before watchdog locking. */
#include "stream_lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

static unsigned long fake_ms;
static int inject_start;
static stream_work_guard_t dma;
static int test_clock_gettime(clockid_t clock, struct timespec *ts);
static int test_mutex_lock(pthread_mutex_t *mutex);

#define clock_gettime test_clock_gettime
#define pthread_mutex_lock test_mutex_lock
#include "../src/common/stream_lifecycle.c"
#undef pthread_mutex_lock
#undef clock_gettime

static int test_clock_gettime(clockid_t clock, struct timespec *ts)
{
    assert(clock == CLOCK_MONOTONIC);
    ts->tv_sec = fake_ms / 1000;
    ts->tv_nsec = (fake_ms % 1000) * 1000000;
    return 0;
}

static int test_mutex_lock(pthread_mutex_t *mutex)
{
    if (inject_start) {
        inject_start = 0;
        ++fake_ms;
        dma = stream_work_begin(STREAM_DMA_WRITE);
        assert(dma.active);
    }
    return pthread_mutex_lock(mutex);
}

int main(void)
{
    assert(!stream_start());
    fake_ms = 100;
    inject_start = 1;
    assert(!stream_check_dma_timeout(1000));
    assert(stream_state() == STREAM_RUNNING);
    fake_ms = 1100;
    assert(!stream_check_dma_timeout(1000));
    fake_ms = 1101;
    assert(stream_check_dma_timeout(1000));
    assert(stream_state() == STREAM_STOPPING);
    stream_work_end(&dma);
    assert(!stream_finish_stop());
    puts("watchdog clock race regression passed");
    return 0;
}
