#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ResumablePhaseStepRetry,
    ResumablePhaseStepAdvance,
    ResumablePhaseStepAdvanceAndRetry,
    ResumablePhaseStepComplete,
} ResumablePhaseStepResult;

typedef ResumablePhaseStepResult (*ResumablePhaseHandler)(void* context, uint8_t phase);

typedef struct {
    uint8_t phase;
} ResumablePhaseMachine;

static inline bool resumable_phase_machine_run(
    ResumablePhaseMachine* machine,
    ResumablePhaseHandler handler,
    void* context) {
    while(true) {
        ResumablePhaseStepResult result = handler(context, machine->phase);
        switch(result) {
        case ResumablePhaseStepRetry:
            return false;
        case ResumablePhaseStepAdvance:
            machine->phase++;
            break;
        case ResumablePhaseStepAdvanceAndRetry:
            machine->phase++;
            return false;
        case ResumablePhaseStepComplete:
            return true;
        }
    }
}
