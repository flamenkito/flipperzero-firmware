#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_version.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_airbridge.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/stream/stream.h>

#include <bt/bt_service/bt.h>
#include <extra_profiles/airbridge_profile.h>

#define TAG "AirBridge"

static const bool airbridge_ble_enabled = true; // flip to false to skip BLE install for bisect

#define EVENT_TYPE_INPUT (1 << 0)
#define EVENT_TYPE_USB   (1 << 1)
#define EVENT_TYPE_BLE   (1 << 2)
#define EVENT_TYPE_RELAY (1 << 3)

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

#define BLE_DEFAULT_NAME           "HP 725 K+M"
#define BLE_DEFAULT_APPEARANCE     0x03C1
#define BLE_DEFAULT_MFG_COMPANY    0x0065
#define BLE_DEFAULT_DIS_MFR        "HP"
#define BLE_DEFAULT_DIS_MODEL      "HP 725 K+M"
#define BLE_DEFAULT_DIS_SERIAL     "HP5341KBD01"
#define BLE_DEFAULT_DIS_PNP        0x0126
#define BLE_SCAN_RESPONSE_OVERHEAD 27U

typedef enum {
    AirbridgeScreenBridge,
    AirbridgeScreenDeployPrompt,
    AirbridgeScreenTyping,
    AirbridgeScreenWaiting,
    AirbridgeScreenStreaming,
    AirbridgeScreenDone,
    AirbridgeScreenError,
} AirbridgeScreen;

typedef enum {
    AirbridgeTypingTransportNone,
    AirbridgeTypingTransportUsb,
    AirbridgeTypingTransportBle,
} AirbridgeTypingTransport;

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

typedef struct {
    FuriMessageQueue* event_queue;
    FuriMessageQueue* input_queue;
    FuriMessageQueue* back_queue;
    uint32_t next_input_sequence;
    bool running;
    bool have_back_head;
    BridgeEvent back_head;
    bool have_input_head;
    BridgeEvent input_head;
    Storage* storage;
    File* io_file;
    File* stream_file;
    Bt* bt;
    FuriHalBleProfileBase* ble_profile;
    AirbridgeBleIdentityParams ble_identity;
    FuriHalUsbInterface* usb_mode_prev;
    AirbridgeScreen screen;
    AirbridgeTypingTransport typing_transport;
    uint8_t profile_index;
    bool usb_configured;
    bool ble_profile_installed;
    bool ble_identity_warning;
    /* Last HIDS-advertising state actually commanded through app_set_hids_adv;
     * makes the setter idempotent. memset-zero matches gap_init's
     * advertise_hids=false, so the startup OFF latch is a correct no-op. */
    bool hids_adv_active;
    bool stream_open;
    bool stream_header_pending;
    uint32_t stream_total_len;
    uint32_t stream_checksum;
    uint32_t stream_sent;
    uint8_t stream_tx_strikes;
    char* bootstrap;
    size_t bootstrap_len;
    size_t typing_position;
    uint16_t typing_key;
    bool typing_key_down;
    bool typing_enter_pending;
    bool typing_enter_done;
    uint32_t typing_next_tick;
    uint32_t typing_jitter_state;
    /* First tick at which the BLE HID link was bonded AND up. Typing emission
     * holds until BLE_TYPE_LINK_SETTLE_MS past this stamp: the macOS HID
     * daemon subscribes the report CCCDs a beat after pairing completes, and
     * keystrokes emitted into that window are silently dropped (hardware:
     * first ~24 bootstrap chars lost on a fresh pairing). */
    uint32_t typing_link_ready_tick;
    /* Bumped by app_start_typing/app_abort_typing; the BLE retry helper
     * captures it at entry and refuses to send once it no longer matches. */
    uint32_t typing_generation;
    /* Deferred BLE release-all (see app_abort_typing): serviced one direct
     * attempt per main-loop iteration, never through the retry wrapper. */
    bool release_all_pending;
    uint8_t release_all_attempts;
    /* Written on the BtSrv thread via app_ble_status_changed_callback, read on
     * the main and GUI threads — volatile so no reader caches a stale value. */
    volatile bool ble_connected;
    uint32_t ble_connected_since;
    uint32_t ble_desync_since;
    /* Last accepted RX write on the current link; zeroed on each new
     * connection. The squatter kick window restarts from this stamp. */
    uint32_t ble_last_rx_tick;
    /* Stamp of the last real 0x42 deploy stream start; zero means no stream
     * this session. Used by the Done-screen zombie-kick. */
    uint32_t stream_started_tick;
    /* Tick of the last Done-screen entry. The zombie-kick waits
     * BLE_DONE_ZOMBIE_GRACE_MS past it: Done is entered when the final chunk
     * is QUEUED in the BLE stack, and an immediate bt_disconnect drops the
     * unsent tail (hardware: bootstrap saw [gattserverdisconnected] +
     * 'Transfer failed' right after a fully-sent stream). The grace lets
     * in-flight notifications flush; the bootstrap's own confirmed
     * disconnect (2500 ms) usually lands first anyway. */
    uint32_t done_since;
    uint32_t ble_waiting_last_pump_tick;
    uint32_t ble_bridge_last_watchdog_tick;
    char error[32];
} AirbridgeApp;

static uint32_t chunks_usb_to_ble;
static uint32_t chunks_ble_to_usb;
static uint32_t dropped;
static uint32_t tx_errors;
static uint32_t vendor_out_requests;
/* Written by the main loop, read by the GUI render thread. */
static volatile bool usb_connected;
static bool usb_config_error;
static uint32_t last_heartbeat;

static void usb_event_callback(HidVendorEvent ev, void* context);
static uint16_t ble_raw_serial_callback(const uint8_t* data, uint16_t len, void* context);
static void app_ble_status_changed_callback(BtStatus status, void* context);

/* Exported firmware symbols (api_symbols.csv); the service header is not in
 * the FAP SDK include path, so declare the two entry points used here. */
typedef struct BleServiceAirbridgeSerial BleServiceAirbridgeSerial;
extern BleServiceAirbridgeSerial* ble_svc_airbridge_serial_get_active(void);
extern bool ble_svc_airbridge_serial_client_subscribed(BleServiceAirbridgeSerial* service);

static void app_set_hids_adv(AirbridgeApp* app, bool enable) {
    /* Idempotent: every state change enqueues a GAP advertising refresh, and
     * the GAP thread holds state_mutex across the whole refresh (several
     * synchronous HCI round-trips to core2 per gap_advertise_start). Every
     * redundant toggle therefore costs the main loop a blocked mutex acquire
     * inside app_service_input — rapid carousel laps chained enough of them
     * (prompt enter/leave plus the redundant OK/abort/BACK/teardown calls) to
     * wedge input processing, long BACK included, behind advertising churn.
     * hids_adv_active is latched only when the HAL is actually commanded, so
     * it always mirrors gap->advertise_hids and the guard never eats a genuine
     * transition. */
    if(enable == app->hids_adv_active) return;
    /* Mid-link the GAP stops advertising and the typing-end OFF re-latches
     * name-only before adv could air again, so an ON swap while connected is
     * pointless. OFF is never pointless: gap_set_adv_hids latches under the GAP
     * mutex in every state and applies at the next adv start, pre-staging
     * name-only advertising for when the link drops (bt_disconnect does not wait
     * for the disconnection-complete event, so ble_connected is still true right
     * after it — a guarded OFF there would leak HIDS into the Waiting adv). The
     * idempotency guard preserves that: tracked state is true only when HIDS
     * adv was really commanded, so a genuine OFF always reaches the latch. */
    if(enable && app->ble_connected) return;
    furi_hal_bt_set_adv_hids(enable);
    /* start_advertising on the ON path only: OFF either refreshes live
     * advertising (set_adv_hids above) or pre-stages the latch for the
     * disconnect callback / Waiting pump, which restart adv themselves. */
    if(enable) {
        furi_hal_bt_start_advertising();
    }
    app->hids_adv_active = enable;
}

static void app_show_error(AirbridgeApp* app, const char* message) {
    snprintf(app->error, sizeof(app->error), "%s", message);
    app->screen = AirbridgeScreenError;
}

static void app_stream_close(AirbridgeApp* app) {
    if(app->stream_open) {
        storage_file_close(app->stream_file);
        app->stream_open = false;
    }
}

static void app_service_input(AirbridgeApp* app);

static bool app_ble_kb_report_with_retry(AirbridgeApp* app, uint8_t* report) {
    const uint32_t generation = app->typing_generation;
    uint8_t failures = 0;
    for(uint8_t attempt = 0; attempt < BLE_TYPING_RETRY_MAX; attempt++) {
        /* Service input at every retry boundary so a queued BACK aborts the
         * in-flight keystroke promptly instead of waiting out all retries. */
        app_service_input(app);
        if(app->screen != AirbridgeScreenTyping || generation != app->typing_generation) {
            return false;
        }
        /* Re-check immediately before the send: a report from an aborted
         * generation must never reach the host — queued BACK -> RIGHT -> OK
         * can start a NEW typing session that reuses AirbridgeScreenTyping. */
        if(app->screen != AirbridgeScreenTyping || generation != app->typing_generation) {
            return false;
        }
        bool error = ble_profile_airbridge_kb_report(app->ble_profile, report, 8);
        if(!error) {
            if(failures > 0) {
                FURI_LOG_W(TAG, "BLE kb report succeeded after %u retries", failures);
            }
            return true;
        }
        failures++;
        FURI_LOG_W(TAG, "BLE kb report failed (attempt %u/%u)", attempt + 1, BLE_TYPING_RETRY_MAX);
        if(attempt + 1 < BLE_TYPING_RETRY_MAX) furi_delay_ms(BLE_TYPING_RETRY_DELAY_MS);
    }
    FURI_LOG_E(TAG, "BLE kb report exhausted %u retries", failures);
    return false;
}

static void app_ble_status_changed_callback(BtStatus status, void* context) {
    AirbridgeApp* app = context;
    bool connected = (status == BtStatusConnected);
    if(connected == app->ble_connected) return;

    app->ble_connected = connected;
    if(connected) {
        app->ble_connected_since = furi_get_tick();
        /* A previous link's RX activity must never extend a fresh link. */
        app->ble_last_rx_tick = 0;
        FURI_LOG_D(TAG, "BLE central connected");
    } else {
        // Restart advertising after any client disconnect so Bridge mode does not go silent.
        // app_restore_ble() handles its own teardown; this is safe because
        // furi_hal_bt_start_advertising() is a no-op unless the GAP state is Idle.
        furi_hal_bt_start_advertising();
        FURI_LOG_D(TAG, "BLE central disconnected");
    }
}

static void app_bridge_adv_watchdog(AirbridgeApp* app) {
    /* GAP state via furi_hal_bt_is_active() is the ground truth;
     * furi_hal_bt_start_advertising() is Idle-gated, and with the bt.c
     * disconnect-status fix the stale-ble_connected wedge class is closed —
     * the watchdog must not depend on FAP bookkeeping. */
    if(!app->ble_profile_installed) return;

    uint32_t now = furi_get_tick();
    if(now - app->ble_bridge_last_watchdog_tick < BLE_BRIDGE_ADV_WATCHDOG_MS) return;
    app->ble_bridge_last_watchdog_tick = now;

    /* HIDS must stay in the adv data for the app's whole lifetime (host BT
     * settings discoverability without prompt navigation). Re-arm every tick:
     * gap_set_adv_hids early-outs when the flag is unchanged, so this is free
     * unless something reset the flag. */
    furi_hal_bt_set_adv_hids(true);

    if(furi_hal_bt_is_active()) return;

    /* Bridge mode must never kick a central off the chat link. Require GAP idle
     * before the kick, and rely on furi_hal_bt_start_advertising()'s own
     * GapStateIdle check as a second guard. This recovers the long-idle/relaunch
     * case without disturbing active BLE relay traffic. */
    FURI_LOG_D(TAG, "BLE Bridge watchdog: restart adv from idle");
    furi_hal_bt_start_advertising();
}

static void app_ble_squatter_watchdog(AirbridgeApp* app) {
    uint32_t now = furi_get_tick();
    /* A 4 s fast kick for bonded squatters (round 6, keyed on
     * bt_pairing_in_progress) was tried and REJECTED on hardware: it evicted
     * clients mid-discovery before they could subscribe (two connect attempts
     * died at ~4 s). 15 s is the proven window, pairing ceremonies included;
     * the bt_pairing_in_progress machinery stays in firmware for future use
     * but is deliberately not consulted here. */
    if(app->ble_connected) {
        /* A central that reached the data phase (any RX write, e.g. the 0x42
         * deploy request or chat frames) is progressing — its window restarts
         * from the last write, so a client attaching through a squatter's
         * shared link is never evicted mid-bring-up by the squatter's stale
         * deadline. */
        uint32_t active_since = app->ble_connected_since;
        if(app->ble_last_rx_tick > active_since) active_since = app->ble_last_rx_tick;
        if(now - active_since < BLE_SQUATTER_KICK_MS) return;
        BleServiceAirbridgeSerial* svc = ble_svc_airbridge_serial_get_active();
        if(svc != NULL && ble_svc_airbridge_serial_client_subscribed(svc)) return;
        FURI_LOG_W(
            TAG,
            "BLE squatter kick: unsubscribed link held %lu ms",
            (unsigned long)(now - app->ble_connected_since));
    } else {
        /* Path B (desync self-heal): GAP says Connected but the FAP never saw
         * the connect (stale disconnect-complete raced a handle reuse). Held
         * for the kick window, force a disconnect to resync. */
        if(!furi_hal_bt_is_connected()) {
            app->ble_desync_since = 0;
            return;
        }
        if(app->ble_desync_since == 0) {
            app->ble_desync_since = now;
            return;
        }
        if(now - app->ble_desync_since < BLE_SQUATTER_KICK_MS) return;
        FURI_LOG_W(
            TAG,
            "BLE desync kick: ghost link held %lu ms",
            (unsigned long)(now - app->ble_desync_since));
    }
    /* With honest GAP state, bt_disconnect blocks until the link is truly down,
     * so the follow-up start_advertising always starts clean advertising; a
     * re-grab by the daemon afterwards is a fresh, correctly-signaled
     * connection. */
    bt_disconnect(app->bt);
    furi_hal_bt_start_advertising();
    app->ble_connected_since = now;
    app->ble_desync_since = 0;
}

static void app_abort_typing(AirbridgeApp* app) {
    /* Invalidate any in-flight retry helper from the aborted session: its next
     * boundary check sees the generation bump and returns without sending. */
    app->typing_generation++;
    const bool key_was_down = app->typing_key_down;
    app->typing_key_down = false;
    app->typing_enter_pending = false;
    app->typing_enter_done = false;
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        /* Release-all is DEFERRED to the main loop: the old synchronous send
         * through the retry wrapper could block the abort path for ~5 s under
         * gatt congestion. The flag is latched here, while the aborted
         * session's transport is known — the loop services it off ble_profile
         * alone, never the mutable typing_transport. This function must stay
         * free of report calls so abort latency never depends on gatt. */
        if(key_was_down) {
            app->release_all_pending = true;
        }
    } else if(key_was_down) {
        /* USB release-all stays synchronous: ISR-driven, non-blocking. */
        furi_hal_hid_airbridge_kb_release_all();
    }
}

static uint8_t app_find_profile(const char* label) {
    for(uint8_t index = 0; index < furi_hal_usb_airbridge_profile_count(); index++) {
        const char* candidate = furi_hal_usb_airbridge_profile_label(index);
        if(candidate != NULL && strcmp(label, candidate) == 0) {
            return index;
        }
    }
    return FuriHalUsbAirbridgeProfileHpKbdVendor;
}

static const AirbridgeBleIdentityParams app_ble_identity_default = {
    .device_name = BLE_DEFAULT_NAME,
    .mac_address = {0x3C, 0x52, 0x82, 0x00, 0x00, 0x01},
    .appearance = BLE_DEFAULT_APPEARANCE,
    .manufacturer_data =
        {
            BLE_DEFAULT_MFG_COMPANY & 0xFF,
            BLE_DEFAULT_MFG_COMPANY >> 8,
        },
    .manufacturer_data_len = 2,
    .dis_manufacturer = BLE_DEFAULT_DIS_MFR,
    .dis_model = BLE_DEFAULT_DIS_MODEL,
    .dis_serial = BLE_DEFAULT_DIS_SERIAL,
    .dis_pnp_version = BLE_DEFAULT_DIS_PNP,
};

static const uint8_t app_hp_ouis[][3] = {
    {0x3C, 0x52, 0x82},
    {0x48, 0x0F, 0xCF},
    {0x94, 0x57, 0xA5},
    {0x3C, 0xD9, 0x2B},
    {0xB4, 0xB6, 0x76},
    {0x2C, 0x44, 0xFD},
    {0xA0, 0xD3, 0xC1},
    {0x40, 0xB0, 0x34},
};

static uint32_t app_ble_serial_hash(void) {
    const uint8_t* uid = furi_hal_version_uid();
    size_t uid_size = furi_hal_version_uid_size();
    if(uid == NULL || uid_size == 0) return 0;

    uint32_t hash = 2166136261U;
    for(size_t index = 0; index < uid_size; index++) {
        hash ^= uid[index];
        hash *= 16777619U;
    }
    return hash;
}

static void app_set_default_ble_identity(AirbridgeApp* app) {
    memcpy(&app->ble_identity, &app_ble_identity_default, sizeof(app->ble_identity));

    const uint32_t serial_hash = app_ble_serial_hash();
    if(serial_hash == 0) return;

    snprintf(
        app->ble_identity.dis_serial,
        sizeof(app->ble_identity.dis_serial),
        "HP%08lX",
        (unsigned long)serial_hash);
}

static bool app_config_key_matches(const char* key, size_t key_len, const char* expected) {
    return (strlen(expected) == key_len) && (strncmp(key, expected, key_len) == 0);
}

static int8_t app_hex_nibble(char value) {
    if(value >= '0' && value <= '9') return value - '0';
    if(value >= 'a' && value <= 'f') return value - 'a' + 10;
    if(value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool app_parse_hex_u16(const char* value, size_t value_len, uint16_t* result) {
    if((value_len >= 2) && (value[0] == '0') && ((value[1] == 'x') || (value[1] == 'X'))) {
        value += 2;
        value_len -= 2;
    }
    if((value_len == 0) || (value_len > 4)) return false;

    uint16_t parsed = 0;
    for(size_t index = 0; index < value_len; index++) {
        int8_t nibble = app_hex_nibble(value[index]);
        if(nibble < 0) return false;
        parsed = (parsed << 4) | nibble;
    }
    *result = parsed;
    return true;
}

static bool app_hp_oui_allowed(const uint8_t* mac_address) {
    for(size_t index = 0; index < COUNT_OF(app_hp_ouis); index++) {
        if(memcmp(mac_address, app_hp_ouis[index], sizeof(app_hp_ouis[index])) == 0) {
            return true;
        }
    }
    return false;
}

static bool app_parse_mac(const char* value, size_t value_len, uint8_t* mac_address) {
    if(value_len != 17) return false;

    for(size_t index = 0; index < AIRBRIDGE_BLE_MAC_ADDRESS_LEN; index++) {
        size_t offset = index * 3;
        int8_t high = app_hex_nibble(value[offset]);
        int8_t low = app_hex_nibble(value[offset + 1]);
        if((high < 0) || (low < 0)) return false;
        if((index < AIRBRIDGE_BLE_MAC_ADDRESS_LEN - 1) && (value[offset + 2] != ':')) {
            return false;
        }
        mac_address[index] = (high << 4) | low;
    }
    return app_hp_oui_allowed(mac_address);
}

static bool
    app_copy_config_value(char* target, size_t target_size, const char* value, size_t value_len) {
    if((value_len == 0) || (value_len >= target_size)) return false;
    memcpy(target, value, value_len);
    target[value_len] = '\0';
    return true;
}

static bool
    app_parse_mfg_hex(AirbridgeBleIdentityParams* identity, const char* value, size_t value_len) {
    if((value_len % 2) != 0) return false;

    size_t extra_len = value_len / 2;
    if(extra_len > sizeof(identity->manufacturer_data) - 2U) return false;

    for(size_t index = 0; index < extra_len; index++) {
        int8_t high = app_hex_nibble(value[index * 2]);
        int8_t low = app_hex_nibble(value[index * 2 + 1]);
        if((high < 0) || (low < 0)) return false;
        identity->manufacturer_data[index + 2] = (high << 4) | low;
    }
    identity->manufacturer_data_len = extra_len + 2U;
    return true;
}

static bool app_parse_config_line(AirbridgeApp* app, const char* line) {
    const char* equals = strchr(line, '=');
    if(equals == NULL) return true;

    const char* key = line;
    while((*key == ' ') || (*key == '\t'))
        key++;
    const char* key_end = equals;
    while((key_end > key) && ((key_end[-1] == ' ') || (key_end[-1] == '\t')))
        key_end--;

    const char* value = equals + 1;
    while((*value == ' ') || (*value == '\t'))
        value++;
    const char* value_end = value;
    while((*value_end != '\0') && (*value_end != '\r') && (*value_end != '\n'))
        value_end++;
    while((value_end > value) && ((value_end[-1] == ' ') || (value_end[-1] == '\t'))) {
        value_end--;
    }

    size_t key_len = key_end - key;
    size_t value_len = value_end - value;
    if(app_config_key_matches(key, key_len, "profile")) {
        char label[32];
        if(!app_copy_config_value(label, sizeof(label), value, value_len)) return false;
        app->profile_index = app_find_profile(label);
    } else if(app_config_key_matches(key, key_len, "ble_name")) {
        return app_copy_config_value(
            app->ble_identity.device_name, sizeof(app->ble_identity.device_name), value, value_len);
    } else if(app_config_key_matches(key, key_len, "ble_mac")) {
        return app_parse_mac(value, value_len, app->ble_identity.mac_address);
    } else if(app_config_key_matches(key, key_len, "ble_appearance")) {
        return app_parse_hex_u16(value, value_len, &app->ble_identity.appearance);
    } else if(app_config_key_matches(key, key_len, "ble_mfg_company")) {
        uint16_t company;
        if(!app_parse_hex_u16(value, value_len, &company)) return false;
        app->ble_identity.manufacturer_data[0] = company & 0xFF;
        app->ble_identity.manufacturer_data[1] = company >> 8;
    } else if(app_config_key_matches(key, key_len, "ble_mfg_hex")) {
        return app_parse_mfg_hex(&app->ble_identity, value, value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_mfr")) {
        return app_copy_config_value(
            app->ble_identity.dis_manufacturer,
            sizeof(app->ble_identity.dis_manufacturer),
            value,
            value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_model")) {
        return app_copy_config_value(
            app->ble_identity.dis_model, sizeof(app->ble_identity.dis_model), value, value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_serial")) {
        return app_copy_config_value(
            app->ble_identity.dis_serial, sizeof(app->ble_identity.dis_serial), value, value_len);
    } else if(app_config_key_matches(key, key_len, "ble_dis_pnp")) {
        return app_parse_hex_u16(value, value_len, &app->ble_identity.dis_pnp_version);
    }
    return true;
}

static void app_load_config(AirbridgeApp* app) {
    app->profile_index = FuriHalUsbAirbridgeProfileHpKbdVendor;
    app->ble_identity_warning = false;
    app_set_default_ble_identity(app);

    Stream* stream = file_stream_alloc(app->storage);
    FuriString* line = furi_string_alloc();
    bool config_valid = true;
    if(file_stream_open(stream, APP_DATA_PATH("config"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, line)) {
            if(!app_parse_config_line(app, furi_string_get_cstr(line))) {
                config_valid = false;
            }
        }
    }
    file_stream_close(stream);
    furi_string_free(line);
    stream_free(stream);

    if(strlen(app->ble_identity.device_name) + app->ble_identity.manufacturer_data_len >
       BLE_SCAN_RESPONSE_OVERHEAD) {
        config_valid = false;
    }
    if(!config_valid) {
        app_set_default_ble_identity(app);
        app->ble_identity_warning = true;
    }
}

static uint32_t app_typing_jitter_ms(AirbridgeApp* app) {
    app->typing_jitter_state =
        app->typing_jitter_state * 1103515245U + 12345U + (uint32_t)app->typing_position;
    return (app->typing_jitter_state >> 16U) % (TYPE_JITTER_MAX_MS + 1U);
}

static bool app_configure_ble(AirbridgeApp* app) {
    if(app->ble_profile_installed) return true;

    app->bt = furi_record_open(RECORD_BT);
    app->ble_profile = bt_profile_start(app->bt, ble_profile_airbridge, (void*)&app->ble_identity);
    app->ble_profile_installed = app->ble_profile != NULL;
    if(app->ble_profile_installed) {
        bt_set_status_changed_callback(app->bt, app_ble_status_changed_callback, app);
        /* HIDS stays in the advertising data for the whole app lifetime: the
         * keyboard must be discoverable/pairable from the host's Bluetooth
         * settings at any moment, without navigating to the Deploy prompt
         * first. */
        app_set_hids_adv(app, true);
    }
    return app->ble_profile_installed;
}

static void app_restore_ble(AirbridgeApp* app) {
    if(app->bt == NULL) {
        app->ble_profile_installed = false;
        return;
    }

    /* Disconnect any active BLE connection and wait for the disconnect to
     * actually land BEFORE tearing down the profile: the bounded wait below
     * gives BleEventWorker time to finish processing the disconnect event
     * (500 ms fallback bound). Without it, the async disconnect event fires
     * into bt_on_gap_event_callback (bt.c:459-475) AFTER
     * ble_svc_airbridge_serial_stop has freed the active service and cleared
     * active_airbridge_serial_service, hitting furi_check(serial_svc) on NULL
     * and furi_crash()-ing the device (looks like a freeze). Additionally,
     * bt->current_profile is a dangling pointer at that point, making
     * bt_profile_is_airbridge a use-after-free. ble_connected is only a proxy
     * for the disconnect having landed: the GapEventTypeDisconnected branch
     * (bt.c:459-475) does not itself fire the status callback — the callback
     * fires via the subsequent StopAdvertising path (bt.c:480-483 ->
     * bt.c:771-777) — so if the link dropped while GAP was idle, no status
     * update fires and the full 500 ms elapses, which is safe because the HCI
     * event has long been processed by then. Zero wait when already
     * disconnected: no 200 ms floor. */
    bt_disconnect(app->bt);
    uint32_t start = furi_get_tick();
    while(app->ble_connected && (furi_get_tick() - start < 500))
        furi_delay_ms(10);

    if(!bt_profile_restore_default(app->bt)) {
        FURI_LOG_E(TAG, "Failed to restore default BLE profile");
    }
    bt_set_status_changed_callback(app->bt, NULL, NULL);
    // Profile restore reinitializes BLE even on a reported failure; do not retain install state.
    app->ble_profile_installed = false;
    app->ble_profile = NULL;
    furi_record_close(RECORD_BT);
    app->bt = NULL;
}

static bool app_apply_profile(AirbridgeApp* app, uint8_t profile_index) {
    FuriHalUsbInterface* profile = furi_hal_usb_airbridge_get_profile(profile_index);
    if(profile == NULL || !furi_hal_usb_set_config(profile, NULL)) {
        usb_config_error = true;
        return false;
    }

    app->profile_index = profile_index;
    app->usb_configured = true;
    usb_config_error = false;
    return true;
}

static bool app_configure_usb(AirbridgeApp* app, uint8_t profile_index) {
    FuriHalUsbInterface* profile = furi_hal_usb_airbridge_get_profile(profile_index);
    if(profile == NULL) return false;

    if(furi_hal_usb_get_config() != profile) {
        furi_hal_usb_unlock();
        if(!app_apply_profile(app, profile_index)) return false;
    } else {
        app->profile_index = profile_index;
        app->usb_configured = true;
    }

    furi_hal_hid_vendor_set_callback(usb_event_callback, app->event_queue);
    bt_set_raw_serial_callback(ble_raw_serial_callback, app);
    return true;
}

static void app_restore_usb(AirbridgeApp* app) {
    if(!app->usb_configured) return;

    bt_set_raw_serial_callback(NULL, NULL);
    furi_hal_hid_vendor_set_callback(NULL, NULL);
    furi_hal_usb_set_config(app->usb_mode_prev, NULL);
    app->usb_configured = false;
}

static bool app_load_bootstrap(AirbridgeApp* app) {
    const char* filename =
        app->typing_transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js" : "bootstrap.js";
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_APP_DATA_PATH_PREFIX, filename);

    if(!storage_file_open(app->io_file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        app_show_error(
            app,
            app->typing_transport == AirbridgeTypingTransportBle ? "NO bootstrap-ble.js ON SD" :
                                                                   "NO bootstrap.js ON SD");
        return false;
    }

    uint64_t file_size = storage_file_size(app->io_file);
    if(file_size == 0) {
        storage_file_close(app->io_file);
        app_show_error(
            app,
            app->typing_transport == AirbridgeTypingTransportBle ? "EMPTY bootstrap-ble.js" :
                                                                   "EMPTY bootstrap.js");
        return false;
    }

    if(app->bootstrap) {
        free(app->bootstrap);
        app->bootstrap = NULL;
    }
    app->bootstrap = malloc(file_size + 1);
    if(app->bootstrap == NULL) {
        storage_file_close(app->io_file);
        app_show_error(
            app,
            app->typing_transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js MALLOC ERR" :
                                                                   "bootstrap.js MALLOC ERR");
        return false;
    }

    app->bootstrap_len = storage_file_read(app->io_file, app->bootstrap, file_size);
    storage_file_close(app->io_file);
    if(app->bootstrap_len != file_size) {
        free(app->bootstrap);
        app->bootstrap = NULL;
        app_show_error(
            app,
            app->typing_transport == AirbridgeTypingTransportBle ? "bootstrap-ble.js READ ERROR" :
                                                                   "bootstrap.js READ ERROR");
        return false;
    }
    app->bootstrap[app->bootstrap_len] = '\0';

    return true;
}

static void app_start_typing(AirbridgeApp* app, AirbridgeTypingTransport transport) {
    /* HID key reports are absolute state: the new session's first report
     * overwrites any stale host-side key state, so a leftover deferred
     * release-all is dead weight — drop it BEFORE anything else. */
    if(app->release_all_pending) {
        app->release_all_pending = false;
        app->release_all_attempts = 0;
        FURI_LOG_W(TAG, "release-all superseded by new typing session");
    }
    app->typing_generation++;
    app->typing_transport = transport;
    if(!app_load_bootstrap(app)) return;
    app->typing_position = 0;
    app->typing_key = HID_KEYBOARD_NONE;
    app->typing_key_down = false;
    app->typing_enter_pending = false;
    app->typing_enter_done = false;
    app->typing_jitter_state = furi_get_tick() ^ ((uint32_t)app->bootstrap_len << 16U) ^
                               app->typing_generation;
    if(app->typing_jitter_state == 0) {
        app->typing_jitter_state = 0xA53C5A5AU;
    }
    app->typing_next_tick = furi_get_tick();
    app->typing_link_ready_tick = 0;
    /* New deploy session: invalidate any stale stream stamp so a later Done
     * screen never zombie-kicks against a previous session's timeline. */
    app->stream_started_tick = 0;
    app->done_since = 0;
    app->screen = AirbridgeScreenTyping;
}

static bool app_typing_press(AirbridgeApp* app, uint16_t key) {
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        if(app->ble_profile == NULL) return false;
        uint8_t report[8] = {
            key >> 8,
            0,
            key & 0xFF,
            0,
            0,
            0,
            0,
            0,
        };
        return app_ble_kb_report_with_retry(app, report);
    }
    return furi_hal_hid_airbridge_kb_press(key);
}

static bool app_typing_release(AirbridgeApp* app, uint16_t key) {
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        if(app->ble_profile == NULL) return false;
        uint8_t report[8] = {0};
        return app_ble_kb_report_with_retry(app, report);
    }
    return furi_hal_hid_airbridge_kb_release(key);
}

static void app_typing_step(AirbridgeApp* app) {
    /* BLE typing holds until the host link is up AND the pairing ceremony has
     * completed. Burning keystrokes into the retry budget before the bonded
     * link exists would error out the session. Input stays serviced in the
     * main loop, so BACK still aborts instantly. */
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        if(!app->ble_connected || bt_pairing_in_progress(app->bt)) {
            app->typing_link_ready_tick = 0;
            app->typing_next_tick = furi_get_tick();
            return;
        }
        if(app->typing_link_ready_tick == 0) {
            app->typing_link_ready_tick = furi_get_tick();
        }
        if(furi_get_tick() - app->typing_link_ready_tick < BLE_TYPE_LINK_SETTLE_MS) {
            app->typing_next_tick = furi_get_tick();
            return;
        }
    }
    if(furi_get_tick() < app->typing_next_tick) return;

    const uint32_t press_delay = app->typing_transport == AirbridgeTypingTransportBle ?
                                     BLE_TYPE_PRESS_DELAY_MS :
                                     TYPE_PRESS_DELAY_MS;
    const uint32_t release_delay = app->typing_transport == AirbridgeTypingTransportBle ?
                                       BLE_TYPE_RELEASE_DELAY_MS :
                                       TYPE_RELEASE_DELAY_MS;

    if(app->typing_key_down) {
        const uint32_t step_generation = app->typing_generation;
        if(!app_typing_release(app, app->typing_key)) {
            /* A false return caused by user abort or by queued input starting a
             * NEW generation must neither error nor abort the new session — so
             * this staleness check runs BEFORE app_abort_typing. */
            if(app->screen != AirbridgeScreenTyping || app->typing_generation != step_generation) {
                return;
            }
            app_abort_typing(app);
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key_down = false;
        if(app->typing_enter_pending) {
            app->typing_enter_pending = false;
            app->typing_enter_done = true;
        } else {
            app->typing_position++;
        }
        uint32_t actual_release_delay = release_delay;
        if(app->typing_transport == AirbridgeTypingTransportBle && (app->typing_key >> 8)) {
            actual_release_delay += BLE_TYPE_MODIFIED_SETTLE_MS;
        }
        actual_release_delay += app_typing_jitter_ms(app);
        app->typing_next_tick = furi_get_tick() + actual_release_delay;
        return;
    }

    if(app->typing_position < app->bootstrap_len) {
        uint16_t key = HID_ASCII_TO_KEY(app->bootstrap[app->typing_position]);
        if(key == HID_KEYBOARD_NONE) {
            app_show_error(app, "bootstrap.js NOT US ASCII");
            return;
        }
        const uint32_t step_generation = app->typing_generation;
        if(!app_typing_press(app, key)) {
            /* Stale-step false return (abort / generation change): no error
             * screen for the session that superseded us. */
            if(app->screen != AirbridgeScreenTyping || app->typing_generation != step_generation) {
                return;
            }
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key = key;
        app->typing_key_down = true;
        app->typing_next_tick = furi_get_tick() + press_delay + app_typing_jitter_ms(app);
        return;
    }

    if(!app->typing_enter_done) {
        const uint32_t step_generation = app->typing_generation;
        if(!app_typing_press(app, HID_KEYBOARD_RETURN)) {
            /* Stale-step false return (abort / generation change): no error
             * screen for the session that superseded us. */
            if(app->screen != AirbridgeScreenTyping || app->typing_generation != step_generation) {
                return;
            }
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key = HID_KEYBOARD_RETURN;
        app->typing_key_down = true;
        app->typing_enter_pending = true;
        app->typing_next_tick = furi_get_tick() + press_delay + app_typing_jitter_ms(app);
        return;
    }

    app->screen = AirbridgeScreenWaiting;
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        app->ble_waiting_last_pump_tick = furi_get_tick();
        bt_disconnect(app->bt);
        /* NO bond wipe here (round-8 wiped; reverted): with ONE identity the
         * typing bond doubles as the browser's data bond — wiping the
         * Flipper-side keys leaves the Mac's bond record pointing at dead
         * keys, and every subsequent connect fails with "Connection attempt
         * failed" (hardware-proven). The daemon's squatting is handled by the
         * squatter watchdog instead. */
        furi_hal_bt_start_advertising();
    }
}

static bool app_start_stream(AirbridgeApp* app) {
    const bool use_ble_bundle = app->typing_transport == AirbridgeTypingTransportBle;
    const char* bundle_path = use_ble_bundle ? APP_DATA_PATH("app-ble.html.gz") :
                                               APP_DATA_PATH("app-usb.html.gz");
    const char* missing_error = use_ble_bundle ? "NO app-ble.gz ON SD" : "NO app-usb.gz ON SD";
    const char* too_large_error = use_ble_bundle ? "app-ble.gz TOO LARGE" : "app-usb.gz TOO LARGE";
    const char* read_error = use_ble_bundle ? "app-ble.gz READ ERROR" : "app-usb.gz READ ERROR";
    if(!storage_file_open(app->stream_file, bundle_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        app_show_error(app, missing_error);
        return false;
    }
    app->stream_open = true;

    uint64_t file_size = storage_file_size(app->stream_file);
    if(file_size > UINT32_MAX) {
        app_stream_close(app);
        app_show_error(app, too_large_error);
        return false;
    }

    uint8_t buffer[HID_VENDOR_PACKET_LEN];
    uint32_t checksum = 0;
    size_t read = 0;
    while((read = storage_file_read(app->stream_file, buffer, sizeof(buffer))) > 0) {
        for(size_t index = 0; index < read; index++) {
            checksum += buffer[index];
        }
    }
    if(!storage_file_seek(app->stream_file, 0, true)) {
        app_stream_close(app);
        app_show_error(app, read_error);
        return false;
    }

    app->stream_total_len = file_size;
    app->stream_checksum = checksum;
    app->stream_sent = 0;
    app->stream_header_pending = true;
    app->stream_tx_strikes = 0;
    app->stream_started_tick = furi_get_tick();
    app->screen = AirbridgeScreenStreaming;
    return true;
}

static void app_stream_step(AirbridgeApp* app) {
    uint8_t report[HID_VENDOR_PACKET_LEN] = {0};
    if(app->stream_header_pending) {
        report[0] = app->stream_total_len & 0xFF;
        report[1] = (app->stream_total_len >> 8) & 0xFF;
        report[2] = (app->stream_total_len >> 16) & 0xFF;
        report[3] = (app->stream_total_len >> 24) & 0xFF;
        report[4] = app->stream_checksum & 0xFF;
        report[5] = (app->stream_checksum >> 8) & 0xFF;
        report[6] = (app->stream_checksum >> 16) & 0xFF;
        report[7] = (app->stream_checksum >> 24) & 0xFF;
        if(!furi_hal_hid_vendor_send_response_blocking(
               report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
            app_stream_close(app);
            app_show_error(app, "STREAM ERROR");
            return;
        }
        app->stream_header_pending = false;
        return;
    }

    if(app->stream_sent == app->stream_total_len) {
        app_stream_close(app);
        app->screen = AirbridgeScreenDone;
        app->done_since = furi_get_tick();
        return;
    }

    uint32_t remaining = app->stream_total_len - app->stream_sent;
    size_t expected = MIN(remaining, sizeof(report));
    size_t read = storage_file_read(app->stream_file, report, expected);
    if(read != expected || !furi_hal_hid_vendor_send_response_blocking(
                               report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
        app_stream_close(app);
        app_show_error(app, "STREAM ERROR");
        return;
    }
    app->stream_sent += read;
}

static void app_stream_step_ble(AirbridgeApp* app) {
    if(app->stream_header_pending) {
        uint8_t header[8] = {
            app->stream_total_len & 0xFF,
            (app->stream_total_len >> 8) & 0xFF,
            (app->stream_total_len >> 16) & 0xFF,
            (app->stream_total_len >> 24) & 0xFF,
            app->stream_checksum & 0xFF,
            (app->stream_checksum >> 8) & 0xFF,
            (app->stream_checksum >> 16) & 0xFF,
            (app->stream_checksum >> 24) & 0xFF,
        };
        if(bt_serial_tx(header, sizeof(header))) {
            app->stream_header_pending = false;
            app->stream_tx_strikes = 0;
        } else {
            app->stream_tx_strikes++;
            if(app->stream_tx_strikes >= BLE_STREAM_RETRY_MAX) {
                app_stream_close(app);
                app_show_error(app, "STREAM STALLED");
            }
        }
        return;
    }

    if(app->stream_sent == app->stream_total_len) {
        app_stream_close(app);
        app->screen = AirbridgeScreenDone;
        app->done_since = furi_get_tick();
        return;
    }

    uint8_t buffer[HID_VENDOR_PACKET_LEN];
    uint32_t remaining = app->stream_total_len - app->stream_sent;
    size_t expected = MIN(remaining, sizeof(buffer));
    size_t read = storage_file_read(app->stream_file, buffer, expected);
    if(read != expected) {
        app_stream_close(app);
        app_show_error(app, "STREAM ERROR");
        return;
    }
    while(!bt_serial_tx(buffer, read)) {
        /* Keep input alive during the spin so BACK aborts the stream within
         * one bt_serial_tx bound (~100 ms) instead of after all retries. If
         * BACK ran app_stream_close, bail WITHOUT touching stream_tx_strikes:
         * the stream file is already closed and the strike book-keeping
         * belongs to a session that no longer exists. */
        app_service_input(app);
        furi_delay_ms(2);
        if(app->screen != AirbridgeScreenStreaming) return;
        app->stream_tx_strikes++;
        if(app->stream_tx_strikes >= BLE_STREAM_RETRY_MAX) {
            app_stream_close(app);
            app_show_error(app, "STREAM STALLED");
            return;
        }
    }
    app->stream_sent += read;
    app->stream_tx_strikes = 0;
}

static void usb_event_callback(HidVendorEvent ev, void* context) {
    FuriMessageQueue* queue = context;
    BridgeEvent be = {0};
    if(ev == HidVendorConnected) {
        be.type = EVENT_TYPE_USB;
        be.to_ble = true;
        if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
            dropped++;
        }
    } else if(ev == HidVendorDisconnected) {
        be.type = EVENT_TYPE_USB;
        be.to_ble = false;
        if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
            dropped++;
        }
    } else if(ev == HidVendorRequest) {
        uint32_t len = furi_hal_hid_vendor_get_request(be.data);
        vendor_out_requests++;
        if(len > 0 && len <= HID_VENDOR_PACKET_LEN) {
            be.type = EVENT_TYPE_RELAY;
            be.len = len;
            be.to_ble = true;
            if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
                dropped++;
            }
        }
    }
}

static uint16_t ble_raw_serial_callback(const uint8_t* data, uint16_t len, void* context) {
    AirbridgeApp* app = context;
    if(len > 0 && len <= HID_VENDOR_PACKET_LEN) {
        app->ble_last_rx_tick = furi_get_tick();
        BridgeEvent be = {
            .type = EVENT_TYPE_RELAY,
            .len = len,
            .to_ble = false,
        };
        memcpy(be.data, data, len);
        if(furi_message_queue_put(app->event_queue, &be, 0) == FuriStatusOk) {
            return HID_VENDOR_PACKET_LEN;
        }
        dropped++;
    }
    return 0;
}

// USB plug outline 7x8 (link down)
static const uint8_t icon_usb_outline[] = {
    0x3E,
    0x55,
    0x41,
    0x3E,
    0x04,
    0x04,
    0x04,
    0x1C,
};
// USB plug filled 7x8 (link up; body solid, contact slits stay holes)
static const uint8_t icon_usb_filled[] = {
    0x3E,
    0x6B,
    0x7F,
    0x3E,
    0x04,
    0x04,
    0x04,
    0x1C,
};
// BT rune 5x8 (from stock Bluetooth_Idle_5x8)
static const uint8_t icon_bt_rune[] = {
    0x04,
    0x0D,
    0x16,
    0x0C,
    0x0C,
    0x16,
    0x0D,
    0x04,
};
// Right arrow 5x5
static const uint8_t icon_arrow_r[] = {
    0x04,
    0x08,
    0x1F,
    0x08,
    0x04,
};
// Trash can 8x8 (DROP)
static const uint8_t icon_trash[] = {
    0x7E,
    0x81,
    0x55,
    0x55,
    0x55,
    0x55,
    0x81,
    0x7E,
};
// Alert triangle 9x8 (TXERR, from stock Alert_9x8)
static const uint8_t icon_alert[] = {
    0x10,
    0x00,
    0x38,
    0x00,
    0x28,
    0x00,
    0x6C,
    0x00,
    0x6C,
    0x00,
    0xFE,
    0x00,
    0xEE,
    0x00,
    0xFF,
    0x01,
};

static void draw_usb_glyph(Canvas* canvas, uint8_t x, uint8_t y, bool connected) {
    canvas_draw_xbm(canvas, x, y, 7, 8, connected ? icon_usb_filled : icon_usb_outline);
}

static void draw_bt_glyph(Canvas* canvas, uint8_t x, uint8_t y, bool connected) {
    canvas_draw_xbm(canvas, x, y, 5, 8, icon_bt_rune);
    if(connected) {
        // Solid pedestal over the sparse bottom rows: "filled" rune, same 8px footprint.
        canvas_draw_box(canvas, x, y + 6, 5, 2);
    }
}

static void draw_identity(Canvas* canvas, AirbridgeApp* app, uint8_t y) {
    if(app->ble_identity_warning) {
        canvas_draw_str(canvas, 0, y, "WARN: BLE ID DEFAULT");
        return;
    }
    const char* identity = furi_hal_usb_airbridge_profile_identity(app->profile_index);
    char line[32];
    snprintf(line, sizeof(line), "HID: %s", identity != NULL ? identity : "--");
    canvas_draw_str(canvas, 0, y, line);
}

static void render_bridge(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);

    // Icon header row (y=24): direction composites + trash + alert.
    // Glyphs encode live link state: filled = connected, outline = down.
    draw_usb_glyph(canvas, 0, 24, usb_connected);
    canvas_draw_xbm(canvas, 9, 26, 5, 5, icon_arrow_r);
    draw_bt_glyph(canvas, 16, 24, app->ble_connected);

    draw_bt_glyph(canvas, 32, 24, app->ble_connected);
    canvas_draw_xbm(canvas, 39, 26, 5, 5, icon_arrow_r);
    draw_usb_glyph(canvas, 46, 24, usb_connected);

    canvas_draw_xbm(canvas, 64, 24, 8, 8, icon_trash);
    canvas_draw_xbm(canvas, 96, 24, 9, 8, icon_alert);

    char line[16];
    snprintf(line, sizeof(line), "%lu", chunks_usb_to_ble);
    canvas_draw_str_aligned(canvas, 0, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", chunks_ble_to_usb);
    canvas_draw_str_aligned(canvas, 32, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", dropped);
    canvas_draw_str_aligned(canvas, 64, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", tx_errors);
    canvas_draw_str_aligned(canvas, 96, 35, AlignLeft, AlignTop, line);
}

static void render_deploy_prompt(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        app->typing_transport == AirbridgeTypingTransportUsb ? "USB Deploy" : "BLE Deploy");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    if(app->typing_transport == AirbridgeTypingTransportUsb) {
        canvas_draw_str(canvas, 0, 31, "Place cursor in browser");
        canvas_draw_str(canvas, 0, 42, "console, then press OK");
    } else {
        canvas_draw_str(canvas, 0, 31, "Pair \"HP 725 K+M\" in BT");
        canvas_draw_str(canvas, 0, 42, "settings, then press OK");
    }
}

static void draw_progress_bar(Canvas* canvas, uint8_t y, uint32_t pos, uint32_t total) {
    canvas_draw_frame(canvas, 4, y, 120, 8);
    if(total == 0) return;
    uint32_t fill_w = 118 * pos / total;
    if(fill_w > 118) fill_w = 118;
    canvas_draw_box(canvas, 5, y + 1, fill_w, 6);
}

static void render_typing(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        app->typing_transport == AirbridgeTypingTransportUsb ? "TYPING via USB" :
                                                               "TYPING via BLE");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    draw_progress_bar(canvas, 28, app->typing_position, app->bootstrap_len);

    char line[32];
    snprintf(
        line,
        sizeof(line),
        "%lu/%lu chars",
        (unsigned long)app->typing_position,
        (unsigned long)app->bootstrap_len);
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, line);
    uint32_t pct =
        app->bootstrap_len > 0 ? 100 * (uint32_t)app->typing_position / app->bootstrap_len : 0;
    snprintf(line, sizeof(line), "%lu%%", pct);
    canvas_draw_str_aligned(canvas, 124, 45, AlignRight, AlignTop, line);

    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

static void render_streaming(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        app->typing_transport == AirbridgeTypingTransportUsb ? "Serving app via USB" :
                                                               "Serving app via BLE");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    draw_progress_bar(canvas, 28, app->stream_sent, app->stream_total_len);

    char line[32];
    uint32_t sent_t = app->stream_sent * 10 / 1024;
    uint32_t total_t = app->stream_total_len * 10 / 1024;
    snprintf(
        line,
        sizeof(line),
        "%lu.%lu/%lu.%lu KB",
        sent_t / 10,
        sent_t % 10,
        total_t / 10,
        total_t % 10);
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, line);
    uint32_t pct = app->stream_total_len > 0 ? 100 * app->stream_sent / app->stream_total_len : 0;
    snprintf(line, sizeof(line), "%lu%%", pct);
    canvas_draw_str_aligned(canvas, 124, 45, AlignRight, AlignTop, line);

    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

static void render_waiting(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Waiting for browser...");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);

    canvas_draw_frame(canvas, 4, 28, 120, 8);
    uint32_t phase = (furi_get_tick() / 50) % 64;
    uint32_t offset = phase < 32 ? phase : 63 - phase;
    canvas_draw_box(canvas, 5 + offset * (118 - 12) / 31, 29, 12, 6);

    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, "Click Connect in the browser");
    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

static void
    render_message(Canvas* canvas, AirbridgeApp* app, const char* title, const char* detail) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, title);
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    canvas_draw_str(canvas, 0, 36, detail);
    canvas_draw_str(canvas, 0, 63, "BACK: Bridge");
}

static void render_callback(Canvas* canvas, void* context) {
    AirbridgeApp* app = context;
    canvas_clear(canvas);
    switch(app->screen) {
    case AirbridgeScreenBridge:
        render_bridge(canvas, app);
        break;
    case AirbridgeScreenDeployPrompt:
        render_deploy_prompt(canvas, app);
        break;
    case AirbridgeScreenTyping:
        render_typing(canvas, app);
        break;
    case AirbridgeScreenWaiting:
        render_waiting(canvas, app);
        break;
    case AirbridgeScreenStreaming:
        render_streaming(canvas, app);
        break;
    case AirbridgeScreenDone:
        render_message(canvas, app, "Done", "OK or BACK: Bridge");
        break;
    case AirbridgeScreenError:
        render_message(canvas, app, "Deploy error", app->error);
        break;
    }
}

static void input_callback(InputEvent* input_event, void* context) {
    AirbridgeApp* app = context;
    /* Back accepts Press (short-BACK semantics) and Long (exit from any
     * screen); a held BACK enqueues both, the Press landing first. */
    bool is_back = (input_event->key == InputKeyBack) &&
                   (input_event->type == InputTypePress || input_event->type == InputTypeLong);
    bool is_short_non_back = (input_event->key != InputKeyBack) &&
                             (input_event->type == InputTypeShort);
    if(!is_back && !is_short_non_back) return;

    BridgeEvent be = {
        .type = EVENT_TYPE_INPUT,
        .tick = furi_get_tick(),
        .sequence = app->next_input_sequence++,
        .key = input_event->key,
        .input_type = input_event->type,
    };
    if(is_back) {
        FURI_LOG_D(TAG, "BACK enqueued %lu", furi_get_tick());
        if(furi_message_queue_put(app->back_queue, &be, 0) != FuriStatusOk) {
            /* BACK is never dropped: a full back_queue already holds pending
             * BACK presses with the identical screen-leaving effect, so the new
             * press coalesces into them without eviction and without dropped++. */
            FURI_LOG_D(TAG, "BACK coalesced");
        }
    } else if(furi_message_queue_put(app->input_queue, &be, 0) != FuriStatusOk) {
        dropped++;
    }
}

/* The relay loop runs off its own event queue independently of app->screen,
 * so carousel switches never interrupt background USB<->BLE traffic. */
static const struct {
    AirbridgeScreen screen;
    AirbridgeTypingTransport transport;
} app_carousel_states[] = {
    {AirbridgeScreenBridge, AirbridgeTypingTransportNone},
    {AirbridgeScreenDeployPrompt, AirbridgeTypingTransportUsb},
    {AirbridgeScreenDeployPrompt, AirbridgeTypingTransportBle},
};

static void app_carousel_next(AirbridgeApp* app, int dir) {
    int current = 0;
    if(app->screen == AirbridgeScreenDeployPrompt) {
        current = (app->typing_transport == AirbridgeTypingTransportBle) ? 2 : 1;
    }
    const int count = (int)COUNT_OF(app_carousel_states);
    const int next = (current + dir + count) % count;
    const AirbridgeScreen next_screen = app_carousel_states[next].screen;
    const AirbridgeTypingTransport next_transport = app_carousel_states[next].transport;

    if(next_transport == AirbridgeTypingTransportUsb &&
       !furi_hal_usb_airbridge_profile_has_keyboard(app->profile_index)) {
        /* A Vendor-only USB profile cannot type; the USB deploy prompt is unreachable for this profile. */
        app_show_error(app, "Set USB to Kbd+Vendor");
        return;
    }

    app->screen = next_screen;
    app->typing_transport = next_transport;
}

static void app_handle_input(AirbridgeApp* app, InputKey key, InputType type, bool* running) {
    /* Long BACK exits from any screen. A held BACK enqueues its Press first,
     * so the screen's short-BACK semantics (prompt -> Bridge, typing -> abort)
     * run on the way out — harmless. */
    if(key == InputKeyBack && type == InputTypeLong) {
        *running = false;
        return;
    }

    if(app->screen == AirbridgeScreenBridge) {
        if(key == InputKeyLeft) {
            app_carousel_next(app, -1);
        } else if(key == InputKeyRight) {
            app_carousel_next(app, +1);
        } else if(key == InputKeyDown && app->ble_profile_installed) {
            /* Manual BLE reset (demo escape hatch): force-drop any held link
             * (squatter, zombie, desync) and re-advertise immediately. */
            FURI_LOG_W(TAG, "Manual BLE reset");
            bt_disconnect(app->bt);
            furi_hal_bt_start_advertising();
            app->ble_connected_since = furi_get_tick();
            app->ble_desync_since = 0;
        }
        /* Short BACK is a no-op on Bridge so carousel browsing can never
         * exit by accident. */
        return;
    }

    if(app->screen == AirbridgeScreenDeployPrompt) {
        if(key == InputKeyBack) {
            app->typing_transport = AirbridgeTypingTransportNone;
            app->screen = AirbridgeScreenBridge;
        } else if(key == InputKeyOk) {
            app_start_typing(app, app->typing_transport);
        } else if(key == InputKeyLeft) {
            app_carousel_next(app, -1);
        } else if(key == InputKeyRight) {
            app_carousel_next(app, +1);
        }
        return;
    }

    if(app->screen == AirbridgeScreenTyping) {
        if(key == InputKeyBack) {
            app_abort_typing(app);
            app->screen = AirbridgeScreenBridge;
        }
        return;
    }

    if(app->screen == AirbridgeScreenStreaming) {
        if(key == InputKeyBack) {
            app_stream_close(app);
            app->screen = AirbridgeScreenBridge;
        }
        return;
    }

    if(app->screen == AirbridgeScreenWaiting) {
        if(key == InputKeyBack) {
            app->screen = AirbridgeScreenBridge;
        }
        return;
    }

    if(key == InputKeyBack || key == InputKeyOk) {
        app->screen = AirbridgeScreenBridge;
    }
}

/* Drains both input queues in temporal order: heads are merged by
 * (tick, sequence) captured at enqueue time — lower tick wins, ties broken by
 * the monotonic sequence, so same-tick presses keep their press order.
 * furi_message_queue has no peek, so each queue's head is cached in the app
 * struct between steps. BACK events ride their own queue, so relay flooding
 * and non-BACK bursts can never evict them. */
static void app_service_input(AirbridgeApp* app) {
    while(true) {
        if(!app->have_back_head) {
            app->have_back_head =
                (furi_message_queue_get(app->back_queue, &app->back_head, 0) == FuriStatusOk);
        }
        if(!app->have_input_head) {
            app->have_input_head =
                (furi_message_queue_get(app->input_queue, &app->input_head, 0) == FuriStatusOk);
        }
        if(!app->have_back_head && !app->have_input_head) return;

        bool take_back = app->have_back_head &&
                         (!app->have_input_head || (app->back_head.tick < app->input_head.tick) ||
                          (app->back_head.tick == app->input_head.tick &&
                           app->back_head.sequence < app->input_head.sequence));
        InputKey key = take_back ? app->back_head.key : app->input_head.key;
        InputType input_type = take_back ? app->back_head.input_type : app->input_head.input_type;
        if(take_back) {
            app->have_back_head = false;
        } else {
            app->have_input_head = false;
        }

        app_handle_input(app, key, input_type, &app->running);
        if(take_back) {
            FURI_LOG_D(TAG, "BACK handled %lu", furi_get_tick());
        }
    }
}

static void app_handle_relay(AirbridgeApp* app, BridgeEvent* be) {
    const bool deploy_request = (be->len > 0) && (be->data[0] == 0x42);
    if(deploy_request && app->screen == AirbridgeScreenWaiting) {
        app_start_stream(app);
        return;
    }
    if(deploy_request && app->screen != AirbridgeScreenBridge) {
        app_show_error(app, "DEPLOY NOT ARMED");
        return;
    }
    if(be->to_ble) {
        chunks_usb_to_ble++;
        if(!bt_serial_tx(be->data, be->len)) {
            tx_errors++;
        }
    } else {
        uint8_t report[HID_VENDOR_PACKET_LEN] = {0};
        memcpy(report, be->data, be->len);
        if(furi_hal_hid_vendor_send_response(report, HID_VENDOR_PACKET_LEN)) {
            chunks_ble_to_usb++;
        } else {
            tx_errors++;
        }
    }
}

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);
    AirbridgeApp* app = malloc(sizeof(*app));
    if(app == NULL) return -1;
    memset(app, 0, sizeof(*app));
    app->screen = AirbridgeScreenBridge;
    app->profile_index = FuriHalUsbAirbridgeProfileHpKbdVendor;
    chunks_usb_to_ble = 0;
    chunks_ble_to_usb = 0;
    dropped = 0;
    tx_errors = 0;
    usb_connected = false;
    usb_config_error = false;
    last_heartbeat = furi_get_tick();

    /* No NULL checks on the allocs below: OOM is unrecoverable by firmware
     * design — view_port_alloc/storage_file_alloc deref their malloc before
     * returning and furi_message_queue_alloc furi_checks internally, so a
     * failed alloc crashes inside the allocator and caller-side checks would
     * be dead code. The app-struct malloc above keeps its check (plain malloc
     * returns NULL instead of crashing). */
    app->event_queue = furi_message_queue_alloc(8, sizeof(BridgeEvent));
    app->input_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    app->back_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    app->storage = furi_record_open(RECORD_STORAGE);
    app->io_file = storage_file_alloc(app->storage);
    app->stream_file = storage_file_alloc(app->storage);
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, app);
    view_port_input_callback_set(view_port, input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    app_load_config(app);
    app->usb_mode_prev = furi_hal_usb_get_config();

    bool startup_apply_pending = true;
    app->running = true;
    while(app->running) {
        app_service_input(app);

        BridgeEvent be;
        FuriStatus status = furi_message_queue_get(app->event_queue, &be, 10);
        if(status == FuriStatusOk) {
            if(be.type == EVENT_TYPE_RELAY) {
                app_handle_relay(app, &be);
            } else if(be.type == EVENT_TYPE_USB) {
                usb_connected = be.to_ble;
            }
        }

        if(startup_apply_pending) {
            // Apply the USB composite profile FIRST, then install the AirBridge BLE profile.
            // The BLE profile start runs furi_hal_bt_reinit() (core2 reset), which in the old
            // order happened before USB apply and left the vendor OUT endpoint unreachable
            // (USB IN worked, U->B counter stayed 0). Applying USB first lets the host enumerate
            // the composite descriptor before any BLE-side core reset activity can disturb it.
            // For bisect, set airbridge_ble_enabled = false to skip BLE install entirely.
            startup_apply_pending = false;
            if(!app_configure_usb(app, app->profile_index)) {
                app_show_error(app, "USB CONFIG ERROR");
            }
            if(airbridge_ble_enabled && !app_configure_ble(app)) {
                app_show_error(app, "BLE CONFIG ERROR");
                app->running = false;
            }
            if(app->ble_profile_installed) {
                app_set_hids_adv(app, false);
            }
        }

        /* Deferred BLE release-all from app_abort_typing: ONE direct attempt
         * per loop iteration, no retry wrapper — the retry path could block
         * ~1 s per attempt in gatt, which is exactly the abort-latency budget
         * this design removes from the abort path. Serviced off ble_profile
         * alone (never the mutable typing_transport) and BEFORE the screen
         * dispatch so a pending release lands ahead of any new typing step. */
        if(app->release_all_pending && app->ble_profile != NULL) {
            uint8_t report[8] = {0};
            if(!ble_profile_airbridge_kb_report(app->ble_profile, report, 8)) {
                app->release_all_pending = false;
                app->release_all_attempts = 0;
            } else {
                app->release_all_attempts++;
                if(app->release_all_attempts >= 3) {
                    app->release_all_pending = false;
                    FURI_LOG_E(TAG, "release-all FAILED - tap a key on target");
                }
            }
        }

        if(app->screen == AirbridgeScreenBridge) {
            app_bridge_adv_watchdog(app);
            /* Squatter watchdog runs on Bridge: the picker window matters here
             * and a bonded macOS HID daemon otherwise holds the link forever. */
            app_ble_squatter_watchdog(app);
        } else if(app->screen == AirbridgeScreenTyping) {
            /* NO squatter kick on Typing: the HID host link is legitimate AND
             * never subscribes the AirBridge TX CCCD, and typing runs 60-90 s —
             * a 15 s kick would corrupt the typed stream mid-bootstrap. */
            app_typing_step(app);
        } else if(app->screen == AirbridgeScreenStreaming) {
            /* NO squatter kick on Streaming: an active bundle/file transfer is
             * definitionally a subscribed, legitimate client. */
            if(app->typing_transport == AirbridgeTypingTransportBle) {
                app_stream_step_ble(app);
            } else {
                app_stream_step(app);
            }
        } else if(app->screen == AirbridgeScreenWaiting) {
            if(app->typing_transport == AirbridgeTypingTransportBle) {
                uint32_t now = furi_get_tick();
                if(now - app->ble_waiting_last_pump_tick >= BLE_WAITING_PUMP_MS) {
                    app->ble_waiting_last_pump_tick = now;
                    // This starts advertising only from GapStateIdle. During a link or
                    // numeric-comparison pairing, GAP is already active, so never
                    // disconnect it merely to recover a genuinely idle advertiser.
                    furi_hal_bt_set_adv_hids(true);
                    FURI_LOG_D(TAG, "BLE Waiting pump: restart adv if idle");
                    furi_hal_bt_start_advertising();
                }
                /* Squatter kick (Waiting): a bonded macOS HID daemon
                 * auto-reconnects the keyboard and sits on the link without ever
                 * subscribing the AirBridge TX CCCD, keeping the Web Bluetooth
                 * picker empty. Chrome's bootstrap subscribes TX immediately,
                 * so any central still unsubscribed past the deadline is safe to
                 * kick. One kick per connection by design: the next rising edge
                 * re-stamps ble_connected_since. 15 s budget (not shorter): on a
                 * FRESH origin the first bootstrap Connect runs a pairing
                 * ceremony (numeric code shown on the Flipper + human reaction
                 * time) BEFORE the TX subscription lands; hardware showed a 3 s
                 * window killing that first connect mid-pairing. */
                app_ble_squatter_watchdog(app);
                /* Waiting zombie-kick: a SUBSCRIBED zombie from a failed
                 * bootstrap attempt (Chrome's gatt.disconnect() does not
                 * reliably drop the OS link) survives the squatter watchdog
                 * and blocks advertising forever. On Waiting the only legit
                 * link progress is the 0x42 deploy write, and
                 * ble_last_rx_tick is zeroed per connection, so a link with
                 * no RX after 90 s (pairing ceremony included) is dead. */
                if(app->ble_connected && app->ble_last_rx_tick == 0 &&
                   furi_get_tick() - app->ble_connected_since >= BLE_WAITING_ZOMBIE_KICK_MS) {
                    FURI_LOG_W(TAG, "BLE Waiting: kicking subscribed zombie link");
                    bt_disconnect(app->bt);
                    furi_hal_bt_start_advertising();
                }
            }
        } else if(app->screen == AirbridgeScreenDone) {
            /* Zombie-kick: the ONLY client allowed to span the stream is the
             * deploy bootstrap, and it must be disconnected by Done
             * (browser-side gatt.disconnect() is not reliable — a subscribed
             * zombie holds the link invisibly and starves the part-3 picker).
             * Any link still up whose connect predates the stream start is that
             * zombie by definition; a legit next client connects only after
             * Done and gets a fresh timestamp. */
            if(app->ble_connected && app->stream_started_tick != 0 &&
               app->ble_connected_since < app->stream_started_tick &&
               furi_get_tick() - app->done_since >= BLE_DONE_ZOMBIE_GRACE_MS) {
                FURI_LOG_W(TAG, "BLE Done: kicking bootstrap zombie link");
                bt_disconnect(app->bt);
                furi_hal_bt_start_advertising();
                app->ble_connected_since = furi_get_tick();
            }
            /* Done: the picker window matters again (the user may start a new
             * transfer from either page), so a squatter holding the link here
             * wedges Web Bluetooth exactly like on Bridge/Waiting. */
            app_ble_squatter_watchdog(app);
        } else if(app->screen == AirbridgeScreenError) {
            /* Error: same picker-window rationale as Done. */
            app_ble_squatter_watchdog(app);
        }
        /* NO squatter kick on DeployPrompt: the pairing ceremony at the prompt
         * (numeric comparison + human reaction time) can exceed 15 s before the
         * TX CCCD subscription lands, and kicking there aborts the deploy. */
        view_port_update(view_port);

        if(furi_get_tick() - last_heartbeat >= 500) {
            notification_message(notifications, &sequence_blink_green_100);
            last_heartbeat = furi_get_tick();
        }
    }

    app_stream_close(app);
    app_abort_typing(app);
    app_restore_usb(app);
    app_set_hids_adv(app, false);
    /* Drain a still-pending release-all BEFORE app_restore_ble NULLs
     * ble_profile: up to 3 direct attempts 20 ms apart. Worst-case exit stall
     * under sustained gatt congestion ~3 s (3 x ~1 s gatt retry + delays) —
     * explicitly accepted; better than leaving a stuck modifier on target. */
    if(app->release_all_pending && app->ble_profile != NULL) {
        uint8_t report[8] = {0};
        for(uint8_t attempt = 0; attempt < 3; attempt++) {
            if(!ble_profile_airbridge_kb_report(app->ble_profile, report, 8)) break;
            if(attempt + 1 < 3) furi_delay_ms(20);
        }
        app->release_all_pending = false;
    }
    app_restore_ble(app);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    storage_file_free(app->io_file);
    storage_file_free(app->stream_file);
    furi_record_close(RECORD_STORAGE);
    view_port_free(view_port);
    furi_message_queue_free(app->input_queue);
    furi_message_queue_free(app->back_queue);
    furi_message_queue_free(app->event_queue);
    free(app->bootstrap);
    free(app);

    return 0;
}
