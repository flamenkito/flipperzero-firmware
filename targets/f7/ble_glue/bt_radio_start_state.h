#pragma once

#include <stdbool.h>

typedef enum {
    BtRadioStartPhaseReady,
    BtRadioStartPhaseQuiesce,
    BtRadioStartPhaseDeinit,
    BtRadioStartPhaseStopGlue,
    BtRadioStartPhaseCleanupComplete,
} BtRadioStartPhase;

typedef struct {
    BtRadioStartPhase phase;
} BtRadioStartState;

typedef struct {
    void* context;
    bool (*start)(void* context);
    bool (*quiesce)(void* context);
    void (*deinit)(void* context);
    bool (*stop_glue)(void* context);
} BtRadioStartOps;

static inline BtRadioStartState bt_radio_start_state_initial(void) {
    return (BtRadioStartState){.phase = BtRadioStartPhaseReady};
}

static inline bool
    bt_radio_start_state_run(BtRadioStartState* state, const BtRadioStartOps* ops) {
    while(true) {
        switch(state->phase) {
        case BtRadioStartPhaseReady:
            if(ops->start(ops->context)) return true;
            state->phase = BtRadioStartPhaseQuiesce;
            break;
        case BtRadioStartPhaseQuiesce:
            if(!ops->quiesce(ops->context)) return false;
            state->phase = BtRadioStartPhaseDeinit;
            break;
        case BtRadioStartPhaseDeinit:
            ops->deinit(ops->context);
            state->phase = BtRadioStartPhaseStopGlue;
            break;
        case BtRadioStartPhaseStopGlue:
            if(!ops->stop_glue(ops->context)) return false;
            state->phase = BtRadioStartPhaseCleanupComplete;
            return false;
        case BtRadioStartPhaseCleanupComplete:
            return false;
        }
    }
}

static inline bool bt_radio_start_state_cleanup_pending(const BtRadioStartState* state) {
    return state->phase == BtRadioStartPhaseQuiesce || state->phase == BtRadioStartPhaseDeinit ||
           state->phase == BtRadioStartPhaseStopGlue;
}

static inline bool bt_radio_start_state_cleanup_complete(const BtRadioStartState* state) {
    return state->phase == BtRadioStartPhaseCleanupComplete;
}

static inline void bt_radio_start_state_reset(BtRadioStartState* state) {
    state->phase = BtRadioStartPhaseReady;
}
