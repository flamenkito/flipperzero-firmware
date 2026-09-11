#pragma once

#include "flooper_schedule.h"

#define FLOOPER_COMMAND_CAPACITY 8U

typedef struct FlooperPlayer FlooperPlayer;

typedef enum {
    FlooperStateLoading,
    FlooperStatePaused,
    FlooperStatePlaying,
    FlooperStatePatternError,
    FlooperStateSpeakerBusy,
    FlooperStateQuitting,
} FlooperState;
typedef enum {
    FlooperPlayerOk,
    FlooperPlayerMemory,
    FlooperPlayerSelection,
    FlooperPlayerStorage,
    FlooperPlayerSize,
    FlooperPlayerParse,
    FlooperPlayerCompile,
    FlooperPlayerClock,
} FlooperPlayerError;
typedef enum {
    FlooperCommandToggle,
    FlooperCommandRestart,
    FlooperCommandLoadSelected,
    FlooperCommandQuit,
} FlooperCommandType;
typedef struct {
    FlooperCommandType type;
    uint8_t selected_index;
} FlooperCommand;
typedef struct {
    uint32_t generation;
    FlooperState state;
    FlooperPlayerError error;
    FlooperPatternError pattern_error;
    FlooperScheduleError schedule_error;
    uint8_t selected_index;
    char display_name[FLOOPER_ID_BYTES], section_name[FLOOPER_ID_BYTES];
    uint8_t count_labels[FLOOPER_COUNTS_MAX], accent_labels[FLOOPER_COUNTS_MAX];
    uint8_t count_count, accent_count, section_index, count_index, count_label;
    uint16_t cycle_index;
    uint32_t pulse_us;
} FlooperSnapshot;

/* Starts one worker; initial selection is loaded asynchronously, never played.
 * NULL means bounded bootstrap memory admission failed. Call from app thread. */
FlooperPlayer* flooper_player_alloc(uint8_t selected_index);
/* Nonblocking. False means full/closing/invalid command. Toggle is resolved only
 * by the worker. Loading invalidates queued ordinary commands across generations. */
bool flooper_player_send(FlooperPlayer* player, FlooperCommand command);
/* Copies a coherent value; false leaves output untouched. No model pointers. */
bool flooper_player_snapshot(FlooperPlayer* player, FlooperSnapshot* output);
/* Long Back can call this directly from a thread-context input callback, BEFORE
 * attempting any GUI input enqueue. Atomic sticky app/worker exit + zero-wait
 * wakeup; main polls exit_requested at <=20ms. Never call from ISR. */
void flooper_player_request_exit(FlooperPlayer* player);
bool flooper_player_exit_requested(const FlooperPlayer* player);
/* App thread only, after unregistering producers/callbacks. Requests cooperative
 * quit and JOINS before freeing shared resources. An in-flight Storage operation
 * may delay return until that firmware call returns; there is no thread kill. */
void flooper_player_free(FlooperPlayer* player);
