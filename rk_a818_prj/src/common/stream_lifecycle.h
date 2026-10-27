#ifndef STREAM_LIFECYCLE_H
#define STREAM_LIFECYCLE_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Disabled by default: wait for the driver instead of stopping after 1 s. */
#ifndef STREAM_DMA_WATCHDOG_ENABLED
#define STREAM_DMA_WATCHDOG_ENABLED 0
#endif
#define STREAM_DMA_WATCHDOG_MS 1000U
#define STREAM_STOP_TIMEOUT_MS 2000U

typedef enum { STREAM_STOPPED, STREAM_RUNNING, STREAM_STOPPING, STREAM_FAULT } stream_state_t;
typedef enum { STREAM_WORK, STREAM_DMA_READ, STREAM_DMA_WRITE } stream_work_kind_t;
typedef struct { int active; stream_work_kind_t kind; } stream_work_guard_t;

stream_state_t stream_state(void);
int stream_start(void);
void stream_request_stop(void);
void stream_set_fault(void);
int stream_finish_stop(void);
stream_work_guard_t stream_work_begin(stream_work_kind_t kind);
void stream_work_end(stream_work_guard_t *guard);
int stream_wait_idle(unsigned int timeout_ms);
/* Watchdog only closes admission; it cannot cancel a kernel DMA request. */
int stream_check_dma_timeout(unsigned int timeout_ms);
int stream_join_thread(pthread_t thread, unsigned int timeout_ms);

/* GCC cleanup also runs on continue/break/return, including in C++ callers. */
#define STREAM_WORK_GUARD(name, kind) \
    stream_work_guard_t name __attribute__((cleanup(stream_work_end))) = stream_work_begin(kind)

#ifdef __cplusplus
}
#endif
#endif
