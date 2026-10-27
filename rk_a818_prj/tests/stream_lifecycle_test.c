#include "stream_lifecycle.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int entered, release_dma;

/* A kernel write that does not return until explicitly released by the test. */
static void *blocked_dma(void *unused)
{
    (void)unused;
    STREAM_WORK_GUARD(work, STREAM_DMA_WRITE);
    assert(work.active);
    pthread_mutex_lock(&test_lock);
    entered = 1;
    pthread_cond_broadcast(&changed);
    while (!release_dma) pthread_cond_wait(&changed, &test_lock);
    pthread_mutex_unlock(&test_lock);
    return NULL;
}

static pthread_t launch_dma(void)
{
    pthread_t thread;
    entered = release_dma = 0;
    assert(!pthread_create(&thread, NULL, blocked_dma, NULL));
    pthread_mutex_lock(&test_lock);
    while (!entered) pthread_cond_wait(&changed, &test_lock);
    pthread_mutex_unlock(&test_lock);
    return thread;
}

static void release_write(pthread_t thread)
{
    pthread_mutex_lock(&test_lock);
    release_dma = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&test_lock);
    assert(!stream_join_thread(thread, 1000));
}

static void reject_new_work(void)
{
    for (int kind = STREAM_WORK; kind <= STREAM_DMA_WRITE; ++kind) {
        STREAM_WORK_GUARD(work, (stream_work_kind_t)kind);
        assert(!work.active);
    }
}

static void test_close_during_write(void)
{
    assert(!stream_start());
    pthread_t thread = launch_dma();
    stream_request_stop();
    stream_request_stop(); /* duplicate close */
    reject_new_work();
    assert(stream_start() == -EBUSY);
    assert(stream_wait_idle(10) == -ETIMEDOUT);
    /* Automatic retries poll without reopening admission or latching FAULT. */
    for (int retry = 0; retry < 5; ++retry) {
        stream_request_stop();
        assert(stream_state() == STREAM_STOPPING);
        assert(stream_wait_idle(0) == -ETIMEDOUT);
        assert(stream_join_thread(thread, 0) == -ETIMEDOUT);
        assert(stream_start() == -EBUSY);
        reject_new_work();
    }
    assert(stream_finish_stop() == -EBUSY); /* must not reclaim/reset yet */
    assert(stream_join_thread(thread, 10) == -ETIMEDOUT);
    release_write(thread);
    /* Late DMA completion is not permission to start before cleanup finishes. */
    assert(stream_state() == STREAM_STOPPING);
    assert(stream_start() == -EBUSY);
    reject_new_work();
    assert(!stream_wait_idle(0));
    assert(!stream_wait_idle(10));
    assert(!stream_finish_stop());
    assert(stream_state() == STREAM_STOPPED);
}

static void test_watchdog_without_close_command(void)
{
    assert(!stream_start());
    pthread_t thread = launch_dma();
    assert(!stream_check_dma_timeout(10000));
    usleep(20000);
    assert(stream_check_dma_timeout(10));
    assert(stream_state() == STREAM_STOPPING);
    reject_new_work();
    assert(!stream_check_dma_timeout(10)); /* only one transition */
    release_write(thread);
    assert(!stream_wait_idle(10));
    assert(!stream_finish_stop());
}

static void test_scope_cleanup(void)
{
    assert(!stream_start());
    for (int i = 0; i < 10; ++i) {
        STREAM_WORK_GUARD(work, STREAM_WORK);
        assert(work.active);
        if (i < 5) continue;
        break;
    }
    stream_request_stop();
    assert(!stream_wait_idle(10));
    assert(!stream_finish_stop());
}

static void test_fault_is_latched(void)
{
    assert(!stream_start());
    pthread_t thread = launch_dma();
    stream_request_stop();
    assert(stream_wait_idle(10) == -ETIMEDOUT);
    stream_set_fault();
    reject_new_work();
    release_write(thread); /* a late completion must not reopen admission */
    stream_request_stop();
    assert(stream_state() == STREAM_FAULT);
    assert(stream_start() == -EBUSY);
    assert(stream_finish_stop() == -EBUSY);
}

int main(void)
{
    assert(stream_state() == STREAM_STOPPED);
    reject_new_work();
    for (int i = 0; i < 20; ++i) test_close_during_write();
    test_watchdog_without_close_command();
    test_scope_cleanup();
    test_fault_is_latched();
    puts("stream lifecycle tests passed");
    return 0;
}
