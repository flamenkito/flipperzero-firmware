#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi.h>
#include <furi_hal_usb.h>

#include "airbridge_types.h"

typedef struct {
    uint32_t chunks_usb_to_ble;
    uint32_t chunks_ble_to_usb;
    uint32_t dropped;
    uint32_t tx_errors;
    bool usb_connected;
} AirbridgeRelayMetrics;

typedef struct AirbridgeApp AirbridgeApp;

typedef bool (*AirbridgeRelayStartStreamCallback)(AirbridgeApp* app);
typedef void (*AirbridgeRelayShowErrorCallback)(AirbridgeApp* app, const char* message);

typedef struct {
    FuriMessageQueue* event_queue;
    FuriHalUsbInterface* usb_mode_prev;
    AirbridgeRelayMetrics metrics;
    bool usb_configured;
} AirbridgeRelay;

void airbridge_relay_init(AirbridgeRelay* relay);
void airbridge_relay_deinit(AirbridgeRelay* relay);
void airbridge_relay_wake(AirbridgeRelay* relay);
FuriStatus
    airbridge_relay_poll(AirbridgeRelay* relay, BridgeEvent* event, uint32_t timeout);
bool airbridge_relay_configure_usb(
    AirbridgeRelay* relay,
    uint8_t profile_index,
    uint8_t* selected_profile_index);
bool airbridge_relay_restore_usb(AirbridgeRelay* relay);
void airbridge_relay_set_usb_connected(AirbridgeRelay* relay, bool connected);
void airbridge_relay_count_drop(AirbridgeRelay* relay);
void airbridge_relay_metrics_snapshot(
    AirbridgeRelay* relay,
    AirbridgeRelayMetrics* snapshot);
void airbridge_relay_handle(
    AirbridgeRelay* relay,
    BridgeEvent* event,
    AirbridgeScreen screen,
    AirbridgeRelayStartStreamCallback start_stream,
    AirbridgeRelayShowErrorCallback show_error,
    AirbridgeApp* app);
