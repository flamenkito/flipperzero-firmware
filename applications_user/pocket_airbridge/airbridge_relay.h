#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi.h>
#include <furi_hal_usb.h>
#include <cli/cli_vcp.h>

#include "airbridge_types.h"
#include "airbridge_ble.h"

typedef struct {
    uint32_t chunks_usb_to_ble;
    uint32_t chunks_ble_to_usb;
    uint32_t dropped;
    uint32_t tx_errors;
    bool usb_connected;
} AirbridgeRelayMetrics;

typedef enum {
    AirbridgeRelayHandled,
    AirbridgeRelayDeployRequestedUsb,
    AirbridgeRelayDeployRequestedBle,
    AirbridgeRelayDeployNotArmed,
} AirbridgeRelayResult;

typedef struct {
    FuriMessageQueue* event_queue;
    FuriHalUsbInterface* usb_mode_prev;
    FuriHalUsbInterface* usb_mode_owned;
    CliVcp* cli_vcp;
    AirbridgeRelayMetrics metrics;
    bool usb_configured;
    bool usb_transition_dirty;
    bool admission_rejected;
    bool cli_was_enabled;
    bool usb_takeover_active;
    bool usb_lock_held;
} AirbridgeRelay;

void airbridge_relay_init(AirbridgeRelay* relay);
void airbridge_relay_deinit(AirbridgeRelay* relay);
void airbridge_relay_wake(AirbridgeRelay* relay);
FuriStatus airbridge_relay_poll(AirbridgeRelay* relay, BridgeEvent* event, uint32_t timeout);
bool airbridge_relay_configure_usb(
    AirbridgeRelay* relay,
    uint8_t profile_index,
    uint8_t* selected_profile_index);
bool airbridge_relay_restore_usb(AirbridgeRelay* relay);
void airbridge_relay_set_usb_connected(AirbridgeRelay* relay, bool connected);
void airbridge_relay_count_drop(AirbridgeRelay* relay);
uint16_t airbridge_relay_ble_event(SerialServiceEvent event, void* context);
void airbridge_relay_metrics_snapshot(AirbridgeRelay* relay, AirbridgeRelayMetrics* snapshot);
AirbridgeRelayResult airbridge_relay_handle(
    AirbridgeRelay* relay,
    AirbridgeBle* ble,
    BridgeEvent* event,
    AirbridgeScreen screen,
    AirbridgeTypingTransport armed_transport);
