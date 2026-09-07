#pragma once

#include <stdbool.h>

typedef enum {
    GapAdvertiseStateIdle,
    GapAdvertiseStateActive,
} GapAdvertiseState;

typedef enum {
    GapAdvertiseStartRadioRejected,
    GapAdvertiseStartTimerRejected,
    GapAdvertiseStartReady,
} GapAdvertiseStartResult;

static inline GapAdvertiseStartResult gap_advertise_state_after_radio_start(
    GapAdvertiseState* state,
    bool radio_started,
    bool timer_started) {
    if(!radio_started) {
        *state = GapAdvertiseStateIdle;
        return GapAdvertiseStartRadioRejected;
    }
    if(!timer_started) {
        *state = GapAdvertiseStateIdle;
        return GapAdvertiseStartTimerRejected;
    }
    *state = GapAdvertiseStateActive;
    return GapAdvertiseStartReady;
}
