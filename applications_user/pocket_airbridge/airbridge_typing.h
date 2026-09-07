#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_ble.h"
#include "airbridge_types.h"

typedef struct AirbridgeApp AirbridgeApp;
typedef void (*AirbridgeTypingServiceInput)(AirbridgeApp* app);
typedef void (*AirbridgeTypingShowError)(AirbridgeApp* app, const char* message);

typedef struct {
    File* io_file;
    AirbridgeBle* ble;
    AirbridgeScreen* screen;
    uint32_t* stream_started_tick;
    uint32_t* done_since;
    AirbridgeApp* app;
    AirbridgeTypingServiceInput service_input;
    AirbridgeTypingShowError show_error;
    AirbridgeTypingTransport transport;
    char* bootstrap;
    size_t bootstrap_len;
    size_t position;
    uint16_t key;
    bool key_down;
    bool enter_pending;
    bool enter_done;
    uint32_t next_tick;
    uint32_t jitter_state;
    /* First tick at which the BLE HID link was bonded AND up. Typing emission
     * holds until BLE_TYPE_LINK_SETTLE_MS past this stamp: the macOS HID
     * daemon subscribes the report CCCDs a beat after pairing completes, and
     * keystrokes emitted into that window are silently dropped (hardware:
     * first ~24 bootstrap chars lost on a fresh pairing). */
    uint32_t link_ready_tick;
    uint32_t disconnected_since;
    /* Bumped by app_start_typing/app_abort_typing; the BLE retry helper
     * captures it at entry and refuses to send once it no longer matches. */
    uint32_t generation;
    /* Deferred BLE release-all (see app_abort_typing): serviced one direct
     * attempt per main-loop iteration, never through the retry wrapper. */
    bool release_all_pending;
    uint8_t release_all_attempts;
} AirbridgeTyping;

void airbridge_typing_init(
    AirbridgeTyping* typing,
    Storage* storage,
    AirbridgeBle* ble,
    AirbridgeScreen* screen,
    uint32_t* stream_started_tick,
    uint32_t* done_since,
    AirbridgeTypingServiceInput service_input,
    AirbridgeTypingShowError show_error,
    AirbridgeApp* app);
void airbridge_typing_release_file(AirbridgeTyping* typing);
void airbridge_typing_deinit(AirbridgeTyping* typing);
void airbridge_typing_start(AirbridgeTyping* typing, AirbridgeTypingTransport transport);
void airbridge_typing_step(AirbridgeTyping* typing);
bool airbridge_typing_abort(AirbridgeTyping* typing);
bool airbridge_typing_service_release(AirbridgeTyping* typing);
void airbridge_typing_drain_release(AirbridgeTyping* typing);
