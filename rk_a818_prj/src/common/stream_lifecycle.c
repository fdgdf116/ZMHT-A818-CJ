#include "stream_lifecycle.h"
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static stream_state_t state = STREAM_STOPPED;
static unsigned int workers;
static unsigned int dma_workers[2];
static uint64_t dma_started[2];

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

stream_state_t stream_state(void)
{
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

int stream_start(void)
{
    int ret = 0;
    pthread_mutex_lock(&lock);
    if (state == STREAM_STOPPED && !workers)
        __atomic_store_n(&state, STREAM_RUNNING, __ATOMIC_RELEASE);
    else if (state != STREAM_RUNNING)
        ret = -EBUSY;
    pthread_mutex_unlock(&lock);
    return ret;
}

void stream_request_stop(void)
{
    pthread_mutex_lock(&lock);
    if (state != STREAM_FAULT)
        __atomic_store_n(&state, STREAM_STOPPING, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
}

void stream_set_fault(void)
{
    pthread_mutex_lock(&lock);
    __atomic_store_n(&state, STREAM_FAULT, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
}

int stream_finish_stop(void)
{
    int ret = -EBUSY;
    pthread_mutex_lock(&lock);
    if (state == STREAM_STOPPING && !workers) {
        __atomic_store_n(&state, STREAM_STOPPED, __ATOMIC_RELEASE);
        ret = 0;
    }
    pthread_mutex_unlock(&lock);
    return ret;
}

stream_work_guard_t stream_work_begin(stream_work_kind_t kind)
{
    stream_work_guard_t guard = {0, kind};
    pthread_mutex_lock(&lock);
    if (state == STREAM_RUNNING) {
        guard.active = 1;
        ++workers;
        if (kind != STREAM_WORK && !dma_workers[kind - STREAM_DMA_READ]++)
            dma_started[kind - STREAM_DMA_READ] = monotonic_ms();
    }
    pthread_mutex_unlock(&lock);
    return guard;
}

void stream_work_end(stream_work_guard_t *guard)
{
    if (!guard->active)
        return;
    pthread_mutex_lock(&lock);
    --workers;
    if (guard->kind != STREAM_WORK)
        --dma_workers[guard->kind - STREAM_DMA_READ];
    guard->active = 0;
    pthread_mutex_unlock(&lock);
}

int stream_wait_idle(unsigned int timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&lock);
        int idle = !workers;
        pthread_mutex_unlock(&lock);
        if (idle)
            return 0;
        if (monotonic_ms() >= deadline)
            return -ETIMEDOUT;
        usleep(1000);
    }
}

int stream_check_dma_timeout(unsigned int timeout_ms)
{
    int expired = 0;
    pthread_mutex_lock(&lock);
    /* Sample under the same lock as dma_started: a newer start timestamp
     * would otherwise underflow the unsigned elapsed-time subtraction. */
    uint64_t now = monotonic_ms();
    if (state == STREAM_RUNNING) {
        for (int i = 0; i < 2; ++i) {
            if (dma_workers[i] && now - dma_started[i] >= timeout_ms) {
                __atomic_store_n(&state, STREAM_STOPPING, __ATOMIC_RELEASE);
                expired = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return expired;
}

int stream_join_thread(pthread_t thread, unsigned int timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    int ret;
    while ((ret = pthread_tryjoin_np(thread, NULL)) == EBUSY) {
        if (monotonic_ms() >= deadline)
            return -ETIMEDOUT;
        usleep(1000);
    }
    return -ret;
}
