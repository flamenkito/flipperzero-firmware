#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FURI_PACKED __attribute__((packed))
#define FURI_CRITICAL_ENTER() \
    do {                      \
    } while(0)
#define FURI_CRITICAL_EXIT() \
    do {                     \
    } while(0)
#define UNUSED(value)         ((void)(value))
#define COUNT_OF(value)       (sizeof(value) / sizeof((value)[0]))
#define furi_check(condition) assert(condition)
#define FURI_LOG_D(...) \
    do {                \
    } while(0)
#define FURI_LOG_W(...) \
    do {                \
    } while(0)
#define FURI_LOG_E(...) \
    do {                \
    } while(0)

typedef enum {
    FuriStatusOk,
    FuriStatusErrorTimeout
} FuriStatus;
typedef struct FuriSemaphore FuriSemaphore;
typedef struct FuriString FuriString;
typedef struct FuriPubSub FuriPubSub;
typedef struct FuriMessageQueue FuriMessageQueue;

uint32_t furi_get_tick(void);
void furi_delay_ms(uint32_t milliseconds);
void* furi_record_open(const char* name);
void furi_record_close(const char* name);

FuriSemaphore* furi_semaphore_alloc(uint32_t maximum, uint32_t initial);
void furi_semaphore_free(FuriSemaphore* semaphore);
FuriStatus furi_semaphore_acquire(FuriSemaphore* semaphore, uint32_t timeout);
FuriStatus furi_semaphore_release(FuriSemaphore* semaphore);
