#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airbridge_usb.h"
#include <input/input.h>

#define EVENT_TYPE_INPUT (1 << 0)
#define EVENT_TYPE_USB   (1 << 1)
#define EVENT_TYPE_RELAY (1 << 2)
#define EVENT_TYPE_WAKE  (1 << 3)

#define TYPE_PRESS_DELAY_MS   12
#define TYPE_RELEASE_DELAY_MS 18
#define TYPE_JITTER_MAX_MS    7
#define STREAM_TIMEOUT_MS     50

#define BOOTSTRAP_MAX_SIZE (16U * 1024U)
#define CONFIG_MAX_SIZE    (4U * 1024U)
#define BUNDLE_MAX_SIZE    (256U * 1024U)

/* BLE Deploy link timing (restored from the pre-USB-only design). The 2500 ms
 * settle covers the macOS HID-daemon CCCD beat: the first ~24 bootstrap chars
 * are silently dropped without it. */
#define BLE_TYPE_LINK_SETTLE_MS            2500
#define BLE_TYPING_RETRY_MAX               5
#define BLE_TYPING_RETRY_DELAY_MS          20
#define BLE_TYPING_DISCONNECT_TIMEOUT_MS   15000U
#define BLE_WAITING_PUMP_MS                2500
#define BLE_WAITING_ZOMBIE_KICK_MS         90000
#define BLE_DONE_ZOMBIE_GRACE_MS           4000
#define BLE_STREAM_RETRY_MAX               20

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

typedef enum {
    AirbridgeTypingTransportUsb,
    AirbridgeTypingTransportBle,
} AirbridgeTypingTransport;

/* The stream reuses the deploy selection domain: the prompt confirmed for
 * typing arms the same transport for the subsequent Waiting/stream phase. */
typedef AirbridgeTypingTransport AirbridgeStreamTransport;

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
