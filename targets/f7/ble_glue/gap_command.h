#pragma once

#include <stdbool.h>

typedef enum {
    GapCommandAdvFast,
    GapCommandAdvLowPower,
    GapCommandAdvStop,
    GapCommandForceIdle,
    GapCommandKillThread,
} GapCommand;

static inline bool gap_command_allowed_during_stop(bool stop_requested, GapCommand command) {
    return !stop_requested || command == GapCommandKillThread;
}
