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

/* BLE typing pacing at USB parity (12/18). macOS negotiates ~11.25-15 ms
 * connection intervals for HID keyboards (our profile offers 7.5-45 ms), so a
 * press+release notification pair drains well within one interval at 12/18 ms;
 * the old 40/60 was sized for the worst-case 45 ms CI. Transient queue
 * congestion is absorbed by app_ble_kb_report_with_retry. RISK under hardware
 * test: a missed release notification corrupts the typed stream (stuck
 * modifier) — the full-bootstrap typing test must show ZERO corruption. */
#define BLE_TYPE_PRESS_DELAY_MS     12
#define BLE_TYPE_RELEASE_DELAY_MS   18
#define BLE_TYPE_MODIFIED_SETTLE_MS 10
#define BLE_TYPE_LINK_SETTLE_MS     2500

#define BLE_TYPING_RETRY_MAX       5
#define BLE_TYPING_RETRY_DELAY_MS  20
#define BLE_WAITING_PUMP_MS        2500
#define BLE_BRIDGE_ADV_WATCHDOG_MS BLE_WAITING_PUMP_MS
#define BLE_SQUATTER_KICK_MS       15000
#define BLE_WAITING_ZOMBIE_KICK_MS 90000
#define BLE_DONE_ZOMBIE_GRACE_MS   4000
#define BLE_STREAM_RETRY_MAX       20

#define BLE_TYPING_DISCONNECT_TIMEOUT_MS 15000U
#define BOOTSTRAP_MAX_SIZE                (16U * 1024U)
#define CONFIG_MAX_SIZE                   (4U * 1024U)
#define BUNDLE_MAX_SIZE                   (256U * 1024U)

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
    AirbridgeTypingTransportNone,
    AirbridgeTypingTransportUsb,
    AirbridgeTypingTransportBle,
} AirbridgeTypingTransport;

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
