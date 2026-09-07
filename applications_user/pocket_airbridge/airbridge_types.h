#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <furi_hal_usb_airbridge.h>
#include <input/input.h>

#define EVENT_TYPE_INPUT (1 << 0)
#define EVENT_TYPE_USB   (1 << 1)
#define EVENT_TYPE_RELAY (1 << 2)
#define EVENT_TYPE_WAKE  (1 << 3)

#define TYPE_PRESS_DELAY_MS   12
#define TYPE_RELEASE_DELAY_MS 18
#define TYPE_JITTER_MAX_MS    7
#define STREAM_TIMEOUT_MS     50

#define BLE_BRIDGE_ADV_WATCHDOG_MS 2500
#define BLE_SQUATTER_KICK_MS       15000

#define BOOTSTRAP_MAX_SIZE (16U * 1024U)
#define CONFIG_MAX_SIZE    (4U * 1024U)
#define BUNDLE_MAX_SIZE    (256U * 1024U)

typedef enum {
    AirbridgeScreenBridge,
    AirbridgeScreenDeployPrompt,
    AirbridgeScreenTyping,
    AirbridgeScreenWaiting,
    AirbridgeScreenStreaming,
    AirbridgeScreenDone,
    AirbridgeScreenError,
    AirbridgeScreenFatal,
} AirbridgeScreen;

typedef struct {
    char title[24];
    char detail[32];
    char action[32];
} AirbridgeError;

typedef struct {
    uint32_t type;
    uint32_t tick;
    uint32_t sequence;
    uint8_t data[HID_VENDOR_PACKET_LEN];
    uint16_t len;
    bool to_ble;
    InputKey key;
    InputType input_type;
} BridgeEvent;
