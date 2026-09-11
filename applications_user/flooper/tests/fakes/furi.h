#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    FuriStatusOk,
    FuriStatusErrorTimeout,
    FuriStatusErrorResource
} FuriStatus;
typedef enum {
    FuriMutexTypeNormal
} FuriMutexType;
typedef struct FuriMutex FuriMutex;
typedef struct FuriMessageQueue FuriMessageQueue;
typedef struct FuriThread FuriThread;
typedef struct FuriTimer FuriTimer;
typedef void (*FuriTimerCallback)(void*);
typedef enum {
    FuriTimerTypeOnce,
    FuriTimerTypePeriodic
} FuriTimerType;
#define FuriWaitForever UINT32_MAX
FuriTimer* furi_timer_alloc(FuriTimerCallback callback, FuriTimerType type, void* context);
FuriStatus furi_timer_start(FuriTimer* timer, uint32_t ticks);
FuriStatus furi_timer_stop(FuriTimer* timer);
void furi_timer_free(FuriTimer* timer);
uint32_t furi_ms_to_ticks(uint32_t ms);
typedef int32_t (*FuriThreadCallback)(void*);
FuriMutex* furi_mutex_alloc(FuriMutexType type);
void furi_mutex_free(FuriMutex* mutex);
FuriStatus furi_mutex_acquire(FuriMutex* mutex, uint32_t timeout);
FuriStatus furi_mutex_release(FuriMutex* mutex);
FuriMessageQueue* furi_message_queue_alloc(uint32_t capacity, uint32_t size);
void furi_message_queue_free(FuriMessageQueue* queue);
FuriStatus furi_message_queue_put(FuriMessageQueue* queue, const void* data, uint32_t timeout);
FuriStatus furi_message_queue_get(FuriMessageQueue* queue, void* data, uint32_t timeout);
FuriStatus furi_message_queue_reset(FuriMessageQueue* queue);
FuriThread* furi_thread_alloc_ex(
    const char* name,
    uint32_t stack,
    FuriThreadCallback callback,
    void* context);
void furi_thread_start(FuriThread* thread);
bool furi_thread_join(FuriThread* thread);
void furi_thread_free(FuriThread* thread);
uint32_t furi_get_tick(void);
uint32_t furi_kernel_get_tick_frequency(void);
int32_t furi_kernel_lock(void);
int32_t furi_kernel_restore_lock(int32_t lock);
size_t memmgr_heap_get_max_free_block(void);
bool furi_record_exists(const char* name);
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
void fake_log(const char* tag, const char* format, ...);
#define FURI_LOG_W(...) fake_log(__VA_ARGS__)
