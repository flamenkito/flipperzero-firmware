#pragma once

#include <stdbool.h>
#include <stdatomic.h>

typedef struct {
    atomic_bool requested;
} AirbridgeExitLatch;

#define AIRBRIDGE_EXIT_LATCH_INITIALIZER \
    { .requested = ATOMIC_VAR_INIT(false) }

static inline void airbridge_exit_latch_reset(AirbridgeExitLatch* latch) {
    atomic_store_explicit(&latch->requested, false, memory_order_relaxed);
}

static inline void airbridge_exit_latch_request(AirbridgeExitLatch* latch) {
    atomic_store_explicit(&latch->requested, true, memory_order_relaxed);
}

static inline bool airbridge_exit_latch_requested(AirbridgeExitLatch* latch) {
    return atomic_load_explicit(&latch->requested, memory_order_relaxed);
}

typedef struct {
    bool callback_detached;
} AirbridgeExitContract;

static inline AirbridgeExitContract airbridge_exit_contract_initial(void) {
    return (AirbridgeExitContract){.callback_detached = false};
}

static inline bool
    airbridge_exit_contract_record_detach(AirbridgeExitContract* contract, bool detached) {
    contract->callback_detached = detached;
    return detached;
}

static inline bool airbridge_exit_contract_owner_retained(const AirbridgeExitContract* contract) {
    return !contract->callback_detached;
}

static inline bool airbridge_exit_contract_may_destroy(const AirbridgeExitContract* contract) {
    return contract->callback_detached;
}
