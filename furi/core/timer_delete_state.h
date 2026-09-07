#pragma once

#include <stdbool.h>

typedef enum {
    FuriTimerDeletePhaseSubmitDelete,
    FuriTimerDeletePhaseSubmitFence,
    FuriTimerDeletePhaseAwaitFence,
    FuriTimerDeletePhaseComplete,
} FuriTimerDeletePhase;

typedef struct {
    FuriTimerDeletePhase phase;
    volatile bool fence_complete;
} FuriTimerDeleteState;

typedef bool (*FuriTimerDeleteOperation)(void* context);

typedef struct {
    void* context;
    FuriTimerDeleteOperation submit_delete;
    FuriTimerDeleteOperation submit_fence;
    FuriTimerDeleteOperation fence_complete;
} FuriTimerDeleteOps;

static inline bool
    furi_timer_delete_state_run(FuriTimerDeleteState* state, const FuriTimerDeleteOps* ops) {
    while(true) {
        switch(state->phase) {
        case FuriTimerDeletePhaseSubmitDelete:
            if(!ops->submit_delete(ops->context)) return false;
            state->phase = FuriTimerDeletePhaseSubmitFence;
            break;
        case FuriTimerDeletePhaseSubmitFence:
            if(!ops->submit_fence(ops->context)) return false;
            state->phase = FuriTimerDeletePhaseAwaitFence;
            break;
        case FuriTimerDeletePhaseAwaitFence:
            if(!ops->fence_complete(ops->context)) return false;
            state->phase = FuriTimerDeletePhaseComplete;
            break;
        case FuriTimerDeletePhaseComplete:
            return true;
        }
    }
}
