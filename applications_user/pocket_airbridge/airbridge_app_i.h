#pragma once

#include <storage/storage.h>
#include <notification/notification.h>

#include "airbridge_ble.h"
#include "airbridge_config.h"
#include "airbridge_relay.h"
#include "airbridge_screens.h"
#include "airbridge_stream.h"
#include "airbridge_typing.h"
#include "airbridge_ui.h"
#include "airbridge_operation.h"

typedef struct AirbridgeApp {
    bool running;
    bool exit_requested;
    uint32_t last_heartbeat;
    Storage* storage;
    NotificationApp* notifications;
    AirbridgeOperationMonitor operation;
    AirbridgeBle ble;
    AirbridgeConfig config;
    AirbridgeRelay relay;
    AirbridgeStream stream;
    AirbridgeTyping typing;
    AirbridgeScreens* screens;
    AirbridgeUi* ui;
    /* Last observed BLE link generation. A fresh generation means a new link:
     * the deploy-accepted latch clears so a stale accept can never mask a
     * Waiting zombie on the next connection. */
    uint32_t ble_generation_seen;
} AirbridgeApp;
