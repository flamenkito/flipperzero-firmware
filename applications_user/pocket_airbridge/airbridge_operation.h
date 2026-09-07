#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AirbridgeOperationIdle,
    AirbridgeOperationUsbStart,
    AirbridgeOperationUsbSend,
    AirbridgeOperationBleStart,
    AirbridgeOperationBleSend,
    AirbridgeOperationBleReconnect,
    AirbridgeOperationClosing,
    AirbridgeOperationUsbStop,
    AirbridgeOperationBleStop,
} AirbridgeOperation;

/* Initial conservative budgets. BLE warnings precede the native 33 s HCI
 * assertion; these are progress deadlines, not proof of a hardware fault. */
enum {
    AirbridgeOperationUsbBudgetMs = 5000,
    AirbridgeOperationBleBudgetMs = 20000,
    AirbridgeOperationClosingBudgetMs = 20000,
};

static inline uint32_t airbridge_operation_budget(AirbridgeOperation operation) {
    switch(operation) {
    case AirbridgeOperationUsbStart:
    case AirbridgeOperationUsbSend:
    case AirbridgeOperationUsbStop:
        return AirbridgeOperationUsbBudgetMs;
    case AirbridgeOperationClosing:
        return AirbridgeOperationClosingBudgetMs;
    default:
        return AirbridgeOperationBleBudgetMs;
    }
}

typedef struct {
    AirbridgeOperation operation;
    bool stalled;
} AirbridgeOperationStatus;

typedef struct {
    uint32_t sequence;
    uint32_t operation;
    uint32_t started;
    uint32_t budget;
    uint32_t observed_sequence;
    uint32_t last_poll;
    uint32_t paused;
    bool was_waiting_for_user;
    AirbridgeOperationStatus status;
} AirbridgeOperationMonitor;

/* One transport worker publishes; an independent supervisor observes. An
 * interrupted publication never makes the observer wait for the worker. */
static inline void airbridge_operation_begin(
    AirbridgeOperationMonitor* monitor,
    AirbridgeOperation operation,
    uint32_t now,
    uint32_t budget) {
    __atomic_add_fetch(&monitor->sequence, 1U, __ATOMIC_SEQ_CST);
    __atomic_store_n(&monitor->operation, operation, __ATOMIC_RELAXED);
    __atomic_store_n(&monitor->started, now, __ATOMIC_RELAXED);
    __atomic_store_n(&monitor->budget, budget, __ATOMIC_RELAXED);
    __atomic_add_fetch(&monitor->sequence, 1U, __ATOMIC_RELEASE);
}

static inline void airbridge_operation_end(AirbridgeOperationMonitor* monitor) {
    airbridge_operation_begin(monitor, AirbridgeOperationIdle, 0, 0);
}

static inline void airbridge_operation_start(
    AirbridgeOperationMonitor* monitor,
    AirbridgeOperation operation,
    uint32_t now) {
    airbridge_operation_begin(monitor, operation, now, airbridge_operation_budget(operation));
}

static inline AirbridgeOperationStatus airbridge_operation_observe(
    AirbridgeOperationMonitor* monitor,
    uint32_t now,
    bool waiting_for_user) {
    const uint32_t sequence = __atomic_load_n(&monitor->sequence, __ATOMIC_ACQUIRE);
    if(sequence & 1U) return monitor->status;
    const AirbridgeOperation operation = __atomic_load_n(&monitor->operation, __ATOMIC_RELAXED);
    const uint32_t started = __atomic_load_n(&monitor->started, __ATOMIC_RELAXED);
    const uint32_t budget = __atomic_load_n(&monitor->budget, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(sequence != __atomic_load_n(&monitor->sequence, __ATOMIC_RELAXED)) {
        return monitor->status;
    }
    /* The worker can publish after the caller sampled its clock. */
    if(operation != AirbridgeOperationIdle && (int32_t)(now - started) < 0) {
        return monitor->status;
    }
    if(sequence != monitor->observed_sequence) {
        monitor->observed_sequence = sequence;
        monitor->last_poll = started;
        monitor->paused = 0;
        monitor->was_waiting_for_user = false;
    }
    waiting_for_user =
        waiting_for_user &&
        (operation == AirbridgeOperationBleStart || operation == AirbridgeOperationBleSend ||
         operation == AirbridgeOperationBleReconnect || operation == AirbridgeOperationBleStop ||
         operation == AirbridgeOperationClosing);
    if(waiting_for_user || monitor->was_waiting_for_user) {
        monitor->paused += now - monitor->last_poll;
    }
    monitor->last_poll = now;
    monitor->was_waiting_for_user = waiting_for_user;
    if(!monitor->status.stalled) {
        monitor->status.operation = operation;
        monitor->status.stalled = operation != AirbridgeOperationIdle && !waiting_for_user &&
                                  now - started - monitor->paused >= budget;
    }
    return monitor->status;
}
