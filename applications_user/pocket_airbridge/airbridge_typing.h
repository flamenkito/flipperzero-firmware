#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <storage/storage.h>

#include "airbridge_ble.h"
#include "airbridge_types.h"

typedef void (*AirbridgeTypingShowError)(void* context, const char* message);

typedef struct {
    File* io_file;
    AirbridgeBle* ble;
    void* error_context;
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
    /* First tick at which the BLE HID link was up with pairing settled. Typing
     * holds until BLE_TYPE_LINK_SETTLE_MS past this stamp. */
    uint32_t link_ready_tick;
    uint32_t disconnected_since;
    /* Bumped by start/abort; the BLE retry helper captures it at entry and
     * refuses to send once it no longer matches, so an aborted session never
     * emits into a superseding one. */
    uint32_t generation;
    /* Deferred BLE release-all: latched here while the aborted session's
     * transport is known, serviced one direct attempt per main-loop iteration
     * via airbridge_typing_service_release. Abort itself never blocks on GATT. */
    bool release_all_pending;
    uint8_t release_all_attempts;
} AirbridgeTyping;

void airbridge_typing_init(
    AirbridgeTyping* typing,
    Storage* storage,
    AirbridgeBle* ble,
    AirbridgeTypingShowError show_error,
    void* error_context);
void airbridge_typing_release_file(AirbridgeTyping* typing);
void airbridge_typing_deinit(AirbridgeTyping* typing);
bool airbridge_typing_start(AirbridgeTyping* typing, AirbridgeTypingTransport transport);
bool airbridge_typing_step(AirbridgeTyping* typing);
bool airbridge_typing_abort(AirbridgeTyping* typing);
bool airbridge_typing_service_release(AirbridgeTyping* typing);
