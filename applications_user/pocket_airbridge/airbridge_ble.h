#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi.h>
#include <bt/bt_service/bt.h>
#include "airbridge_profile.h"

#define BLE_BRIDGE_ADV_WATCHDOG_MS 2500
#define BLE_SQUATTER_KICK_MS       15000

typedef struct {
    Bt* bt;
    AirbridgeBleProfileParams profile_params;
    bool ble_profile_installed;
    bool ble_connected;
    bool restart_adv_pending;
    uint32_t ble_connected_since;
    uint32_t ble_last_rx_tick;
    uint32_t ble_desync_since;
    uint32_t link_generation;
    volatile bool published_connected;
    volatile uint32_t published_tick;
    volatile uint32_t published_sequence;
    volatile uint32_t published_connect_count;
    uint32_t consumed_sequence;
    uint32_t consumed_connect_count;
    uint32_t implicit_connect_credit;
    uint32_t ble_bridge_last_watchdog_tick;
} AirbridgeBle;

bool airbridge_ble_ensure_serial_adv(AirbridgeBle* ble);
void airbridge_ble_service_pending(AirbridgeBle* ble);
void airbridge_ble_note_rx(AirbridgeBle* ble, uint32_t tick);
void airbridge_ble_force_reconnect(AirbridgeBle* ble);
void airbridge_ble_bridge_adv_watchdog(AirbridgeBle* ble);
void airbridge_ble_squatter_watchdog(AirbridgeBle* ble);
bool airbridge_ble_configure(
    AirbridgeBle* ble,
    const AirbridgeBleIdentityParams* identity,
    AirbridgeSerialServiceEventCallback callback,
    void* context);
bool airbridge_ble_send(AirbridgeBle* ble, uint8_t* data, uint16_t len);
bool airbridge_ble_restore(AirbridgeBle* ble);
