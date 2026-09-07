#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AIRBRIDGE_EXIT_POLL_INTERVAL_MS 10U

typedef struct {
    uint32_t requested;
} AirbridgeExitLatch;

#define AIRBRIDGE_EXIT_LATCH_INITIALIZER \
    { .requested = 0U }

static inline void airbridge_exit_latch_reset(AirbridgeExitLatch* latch) {
    __atomic_store_n(&latch->requested, 0U, __ATOMIC_RELAXED);
}

static inline void airbridge_exit_latch_request(AirbridgeExitLatch* latch) {
    __atomic_store_n(&latch->requested, 1U, __ATOMIC_RELAXED);
}

static inline bool airbridge_exit_latch_requested(AirbridgeExitLatch* latch) {
    return __atomic_load_n(&latch->requested, __ATOMIC_RELAXED) != 0U;
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
