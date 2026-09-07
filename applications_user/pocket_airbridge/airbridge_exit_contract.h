#pragma once

#include <stdbool.h>

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
