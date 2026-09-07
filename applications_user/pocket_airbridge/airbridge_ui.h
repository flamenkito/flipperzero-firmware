#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi.h>
#include <gui/gui.h>

#include "airbridge_ble.h"
#include "airbridge_config.h"
#include "airbridge_relay.h"
#include "airbridge_stream.h"
#include "airbridge_typing.h"
#include "airbridge_types.h"

typedef struct AirbridgeApp AirbridgeApp;
typedef void (*AirbridgeUiShowError)(AirbridgeApp* app, const char* message);

typedef struct {
    AirbridgeScreen screen;
    AirbridgeTypingTransport transport;
    AirbridgeRelayMetrics metrics;
    bool ble_connected;
    bool identity_warning;
    uint8_t usb_profile_index;
    char ble_name[AIRBRIDGE_BLE_DEVICE_NAME_MAX_LEN + 1U];
    uint64_t typing_position;
    uint64_t typing_total;
    uint64_t stream_sent;
    uint64_t stream_total;
    AirbridgeError error;
} AirbridgeUiSnapshot;

typedef struct {
    FuriMessageQueue* input_queue;
    FuriMessageQueue* back_queue;
    uint32_t next_input_sequence;
    bool have_back_head;
    BridgeEvent back_head;
    bool have_input_head;
    BridgeEvent input_head;
    ViewPort* view_port;
    Gui* gui;
    FuriMutex* snapshot_mutex;
    AirbridgeUiSnapshot snapshot;
    bool* running;
    volatile bool* exit_requested;
    AirbridgeScreen* screen;
    AirbridgeConfig* config;
    AirbridgeRelay* relay;
    AirbridgeBle* ble;
    AirbridgeTyping* typing;
    AirbridgeStream* stream;
    AirbridgeError* error;
    AirbridgeUiShowError show_error;
    AirbridgeApp* app;
} AirbridgeUi;

void airbridge_ui_init(
    AirbridgeUi* ui,
    bool* running,
    volatile bool* exit_requested,
    AirbridgeScreen* screen,
    AirbridgeConfig* config,
    AirbridgeRelay* relay,
    AirbridgeBle* ble,
    AirbridgeTyping* typing,
    AirbridgeStream* stream,
    AirbridgeError* error,
    AirbridgeUiShowError show_error,
    AirbridgeApp* app);
void airbridge_ui_init_view(AirbridgeUi* ui);
void airbridge_ui_open_gui(AirbridgeUi* ui);
void airbridge_ui_add_view(AirbridgeUi* ui);
void airbridge_ui_update(AirbridgeUi* ui);
void airbridge_ui_service_input(AirbridgeUi* ui);
void airbridge_ui_remove_view(AirbridgeUi* ui);
void airbridge_ui_close_gui(void);
void airbridge_ui_free_view(AirbridgeUi* ui);
void airbridge_ui_deinit(AirbridgeUi* ui);
