#include <furi.h>
#include <furi_hal.h>
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

#define BLE_TYPING_RETRY_MAX      5
#define BLE_TYPING_RETRY_DELAY_MS 20
#define BLE_WAITING_PUMP_MS          2500
#define BLE_WAITING_SQUATTER_KICK_MS 15000
#define BLE_STREAM_RETRY_MAX      20

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
    AirbridgeTypingTransportUsb,
    AirbridgeTypingTransportBle,
} AirbridgeTypingTransport;

typedef struct {
    uint32_t type;
    uint8_t data[HID_VENDOR_PACKET_LEN];
    uint16_t len;
    bool to_ble;
    InputKey key;
} BridgeEvent;

typedef struct {
    FuriMessageQueue* event_queue;
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
    bool ble_connected;
    uint32_t ble_connected_since;
    uint32_t ble_waiting_last_pump_tick;
    char error[32];
} AirbridgeApp;

static uint32_t chunks_usb_to_ble;
static uint32_t chunks_ble_to_usb;
static uint32_t dropped;
static uint32_t tx_errors;
static uint32_t vendor_out_requests;
static bool usb_connected;
static bool usb_config_error;
static uint32_t last_heartbeat;

static void usb_event_callback(HidVendorEvent ev, void* context);
static uint16_t ble_raw_serial_callback(const uint8_t* data, uint16_t len, void* context);
static void app_ble_status_changed_callback(BtStatus status, void* context);

static void app_set_hids_adv(AirbridgeApp* app, bool enable) {
    /* Mid-link the GAP stops advertising and the typing-end OFF re-latches
     * name-only before adv could air again, so an ON swap while connected is
     * pointless. OFF is never pointless: gap_set_adv_hids latches under the GAP
     * mutex in every state and applies at the next adv start, pre-staging
     * name-only advertising for when the link drops (bt_disconnect does not wait
     * for the disconnection-complete event, so ble_connected is still true right
     * after it — a guarded OFF there would leak HIDS into the Waiting adv). */
    if(enable && app->ble_connected) return;
    furi_hal_bt_set_adv_hids(enable);
    furi_hal_bt_start_advertising();
}

static void app_show_error(AirbridgeApp* app, const char* message) {
    if((app->screen == AirbridgeScreenTyping ||
        app->screen == AirbridgeScreenDeployPrompt) &&
       app->typing_transport == AirbridgeTypingTransportBle) {
        app_set_hids_adv(app, false);
    }
    snprintf(app->error, sizeof(app->error), "%s", message);
    app->screen = AirbridgeScreenError;
}

static void app_stream_close(AirbridgeApp* app) {
    if(app->stream_open) {
        storage_file_close(app->stream_file);
        app->stream_open = false;
    }
}

static bool app_ble_kb_report_with_retry(FuriHalBleProfileBase* profile, uint8_t* report) {
    uint8_t failures = 0;
    for(uint8_t attempt = 0; attempt < BLE_TYPING_RETRY_MAX; attempt++) {
        bool error = ble_profile_airbridge_kb_report(profile, report, 8);
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
        FURI_LOG_D(TAG, "BLE central connected");
    } else {
        // Restart advertising after any client disconnect so Bridge mode does not go silent.
        // app_restore_ble() handles its own teardown; this is safe because
        // furi_hal_bt_start_advertising() is a no-op unless the GAP state is Idle.
        furi_hal_bt_start_advertising();
        FURI_LOG_D(TAG, "BLE central disconnected");
    }
}

static void app_abort_typing(AirbridgeApp* app) {
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        app_set_hids_adv(app, false);
    }
    if(app->typing_key_down) {
        if(app->typing_transport == AirbridgeTypingTransportBle) {
            uint8_t report[8] = {0};
            if(app->ble_profile) {
                app_ble_kb_report_with_retry(app->ble_profile, report);
            }
        } else {
            furi_hal_hid_airbridge_kb_release_all();
        }
    }
    app->typing_key_down = false;
    app->typing_enter_pending = false;
    app->typing_enter_done = false;
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
    memcpy(&app->ble_identity, &app_ble_identity_default, sizeof(app->ble_identity));

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
        memcpy(&app->ble_identity, &app_ble_identity_default, sizeof(app->ble_identity));
        app->ble_identity_warning = true;
    }
}

static bool app_configure_ble(AirbridgeApp* app) {
    if(app->ble_profile_installed) return true;

    app->bt = furi_record_open(RECORD_BT);
    app->ble_profile = bt_profile_start(app->bt, ble_profile_airbridge, (void*)&app->ble_identity);
    app->ble_profile_installed = app->ble_profile != NULL;
    if(app->ble_profile_installed) {
        bt_set_status_changed_callback(app->bt, app_ble_status_changed_callback, app);
    }
    return app->ble_profile_installed;
}

static void app_restore_ble(AirbridgeApp* app) {
    if(app->bt == NULL) {
        app->ble_profile_installed = false;
        return;
    }

    /* Disconnect any active BLE connection and wait for the HCI
     * DISCONNECTION_COMPLETE event to be fully processed BEFORE tearing down
     * the profile. Without this, the async disconnect event fires into
     * bt_on_gap_event_callback (bt.c:339-344) AFTER
     * ble_svc_airbridge_serial_stop has freed the active service and cleared
     * active_airbridge_serial_service, hitting furi_check(serial_svc) on NULL
     * and furi_crash()-ing the device (looks like a freeze). Additionally,
     * bt->current_profile is a dangling pointer at that point, making
     * bt_profile_is_airbridge a use-after-free. Mirrors the stock hid_app exit
     * (hid.c:232-239): bt_disconnect + 200 ms + bt_profile_restore_default. */
    bt_disconnect(app->bt);
    furi_delay_ms(200);

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
    app->typing_transport = transport;
    if(!app_load_bootstrap(app)) return;
    app->typing_position = 0;
    app->typing_key = HID_KEYBOARD_NONE;
    app->typing_key_down = false;
    app->typing_enter_pending = false;
    app->typing_enter_done = false;
    app->typing_next_tick = furi_get_tick();
    if(transport == AirbridgeTypingTransportBle) {
        app_set_hids_adv(app, true);
    }
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
        return app_ble_kb_report_with_retry(app->ble_profile, report);
    }
    return furi_hal_hid_airbridge_kb_press(key);
}

static bool app_typing_release(AirbridgeApp* app, uint16_t key) {
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        if(app->ble_profile == NULL) return false;
        uint8_t report[8] = {0};
        return app_ble_kb_report_with_retry(app->ble_profile, report);
    }
    return furi_hal_hid_airbridge_kb_release(key);
}

static void app_typing_step(AirbridgeApp* app) {
    if(furi_get_tick() < app->typing_next_tick) return;

    const uint32_t press_delay = app->typing_transport == AirbridgeTypingTransportBle ?
                                     BLE_TYPE_PRESS_DELAY_MS :
                                     TYPE_PRESS_DELAY_MS;
    const uint32_t release_delay = app->typing_transport == AirbridgeTypingTransportBle ?
                                       BLE_TYPE_RELEASE_DELAY_MS :
                                       TYPE_RELEASE_DELAY_MS;

    if(app->typing_key_down) {
        if(!app_typing_release(app, app->typing_key)) {
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
        app->typing_next_tick = furi_get_tick() + actual_release_delay;
        return;
    }

    if(app->typing_position < app->bootstrap_len) {
        uint16_t key = HID_ASCII_TO_KEY(app->bootstrap[app->typing_position]);
        if(key == HID_KEYBOARD_NONE) {
            app_show_error(app, "bootstrap.js NOT US ASCII");
            return;
        }
        if(!app_typing_press(app, key)) {
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key = key;
        app->typing_key_down = true;
        app->typing_next_tick = furi_get_tick() + press_delay;
        return;
    }

    if(!app->typing_enter_done) {
        if(!app_typing_press(app, HID_KEYBOARD_RETURN)) {
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key = HID_KEYBOARD_RETURN;
        app->typing_key_down = true;
        app->typing_enter_pending = true;
        app->typing_next_tick = furi_get_tick() + press_delay;
        return;
    }

    app->screen = AirbridgeScreenWaiting;
    if(app->typing_transport == AirbridgeTypingTransportBle) {
        app_set_hids_adv(app, false);
        app->ble_waiting_last_pump_tick = furi_get_tick();
        bt_disconnect(app->bt);
        furi_hal_bt_start_advertising();
    }
}

static bool app_start_stream(AirbridgeApp* app) {
    const bool use_ble_bundle = app->typing_transport == AirbridgeTypingTransportBle;
    const char* bundle_path = use_ble_bundle ? APP_DATA_PATH("app-ble.html") :
                                               APP_DATA_PATH("app-usb.html");
    const char* missing_error = use_ble_bundle ? "NO app-ble.html ON SD" : "NO app-usb.html ON SD";
    const char* too_large_error = use_ble_bundle ? "app-ble.html TOO LARGE" :
                                                   "app-usb.html TOO LARGE";
    const char* read_error = use_ble_bundle ? "app-ble.html READ ERROR" :
                                              "app-usb.html READ ERROR";
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
    0x3E, 0x55, 0x41, 0x3E, 0x04, 0x04, 0x04, 0x1C,
};
// USB plug filled 7x8 (link up; body solid, contact slits stay holes)
static const uint8_t icon_usb_filled[] = {
    0x3E, 0x6B, 0x7F, 0x3E, 0x04, 0x04, 0x04, 0x1C,
};
// BT rune 5x8 (from stock Bluetooth_Idle_5x8)
static const uint8_t icon_bt_rune[] = {
    0x04, 0x0D, 0x16, 0x0C, 0x0C, 0x16, 0x0D, 0x04,
};
// Right arrow 5x5
static const uint8_t icon_arrow_r[] = {
    0x04, 0x08, 0x1F, 0x08, 0x04,
};
// Trash can 8x8 (DROP)
static const uint8_t icon_trash[] = {
    0x7E, 0x81, 0x55, 0x55, 0x55, 0x55, 0x81, 0x7E,
};
// Alert triangle 9x8 (TXERR, from stock Alert_9x8)
static const uint8_t icon_alert[] = {
    0x10, 0x00, 0x38, 0x00, 0x28, 0x00, 0x6C, 0x00,
    0x6C, 0x00, 0xFE, 0x00, 0xEE, 0x00, 0xFF, 0x01,
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

static void draw_up_arrow(Canvas* canvas, uint8_t x, uint8_t y) {
    canvas_draw_line(canvas, x + 3, y, x, y + 3);
    canvas_draw_line(canvas, x + 3, y, x + 6, y + 3);
    canvas_draw_line(canvas, x + 3, y, x + 3, y + 5);
}

static void draw_down_arrow(Canvas* canvas, uint8_t x, uint8_t y) {
    canvas_draw_line(canvas, x + 3, y, x + 3, y + 5);
    canvas_draw_line(canvas, x, y + 2, x + 3, y + 5);
    canvas_draw_line(canvas, x + 6, y + 2, x + 3, y + 5);
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

    draw_up_arrow(canvas, 0, 47);
    canvas_draw_str(canvas, 9, 53, "USB deploy");
    draw_down_arrow(canvas, 64, 47);
    canvas_draw_str(canvas, 73, 53, "BLE deploy");
    canvas_draw_str(canvas, 0, 63, "BACK: exit");
}

static void render_deploy_prompt(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Deploy app");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    if(app->typing_transport == AirbridgeTypingTransportUsb) {
        canvas_draw_str(canvas, 0, 31, "Place cursor in browser");
        canvas_draw_str(canvas, 0, 42, "console, then press OK");
    } else {
        canvas_draw_str(canvas, 0, 31, "Pair \"HP 725 K+M\" in BT");
        canvas_draw_str(canvas, 0, 42, "settings, then press OK");
    }
    canvas_draw_str(
        canvas,
        0,
        53,
        app->typing_transport == AirbridgeTypingTransportUsb ? "OK types via USB" :
                                                               "OK types via BLE");
    canvas_draw_str(canvas, 0, 63, "BACK: Bridge");
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
    uint32_t pct =
        app->stream_total_len > 0 ? 100 * app->stream_sent / app->stream_total_len : 0;
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

    canvas_draw_str_aligned(
        canvas, 0, 45, AlignLeft, AlignTop, "Click Connect in the browser");
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
    FuriMessageQueue* queue = context;
    bool is_back_press = (input_event->key == InputKeyBack) &&
                         (input_event->type == InputTypePress);
    bool is_short_non_back = (input_event->key != InputKeyBack) &&
                             (input_event->type == InputTypeShort);
    if(!is_back_press && !is_short_non_back) return;

    BridgeEvent be = {
        .type = EVENT_TYPE_INPUT,
        .key = input_event->key,
    };
    if(furi_message_queue_put(queue, &be, 0) != FuriStatusOk) {
        dropped++;
    }
}

static void app_handle_input(AirbridgeApp* app, InputKey key, bool* running) {
    if(app->screen == AirbridgeScreenBridge) {
        if(key == InputKeyBack) {
            *running = false;
        } else if(key == InputKeyUp) {
            if(!furi_hal_usb_airbridge_profile_has_keyboard(app->profile_index)) {
                app_show_error(app, "Set USB to Kbd+Vendor");
            } else {
                app->typing_transport = AirbridgeTypingTransportUsb;
                app->screen = AirbridgeScreenDeployPrompt;
            }
        } else if(key == InputKeyDown) {
            app->typing_transport = AirbridgeTypingTransportBle;
            app_set_hids_adv(app, true);
            app->screen = AirbridgeScreenDeployPrompt;
        }
        return;
    }

    if(app->screen == AirbridgeScreenDeployPrompt) {
        if(key == InputKeyBack) {
            if(app->typing_transport == AirbridgeTypingTransportBle) {
                app_set_hids_adv(app, false);
            }
            app->screen = AirbridgeScreenBridge;
        } else if(key == InputKeyOk) {
            app_start_typing(app, app->typing_transport);
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

static void app_handle_relay(AirbridgeApp* app, BridgeEvent* be) {
    if((app->screen == AirbridgeScreenWaiting) && (be->len > 0) && (be->data[0] == 0x42)) {
        app_start_stream(app);
    } else if(be->to_ble) {
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

    app->event_queue = furi_message_queue_alloc(8, sizeof(BridgeEvent));
    app->storage = furi_record_open(RECORD_STORAGE);
    app->io_file = storage_file_alloc(app->storage);
    app->stream_file = storage_file_alloc(app->storage);
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, app);
    view_port_input_callback_set(view_port, input_callback, app->event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    app_load_config(app);
    app->usb_mode_prev = furi_hal_usb_get_config();

    bool startup_apply_pending = true;
    bool running = true;
    while(running) {
        BridgeEvent be;
        FuriStatus status = furi_message_queue_get(app->event_queue, &be, 10);
        if(status == FuriStatusOk) {
            if(be.type == EVENT_TYPE_RELAY) {
                app_handle_relay(app, &be);
            } else if(be.type == EVENT_TYPE_USB) {
                usb_connected = be.to_ble;
            } else if(be.type == EVENT_TYPE_INPUT) {
                app_handle_input(app, be.key, &running);
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
                running = false;
            }
            if(app->ble_profile_installed) {
                app_set_hids_adv(app, false);
            }
        }

        if(app->screen == AirbridgeScreenTyping) {
            app_typing_step(app);
        } else if(app->screen == AirbridgeScreenStreaming) {
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
                    FURI_LOG_D(TAG, "BLE Waiting pump: restart adv if idle");
                    furi_hal_bt_start_advertising();
                }
                /* Squatter kick (deploy-scoped, BLE Waiting only): a bonded macOS
                 * HID daemon auto-reconnects the keyboard and sits on HIDS without
                 * ever writing the vendor RX, holding the link and keeping the Web
                 * Bluetooth picker empty. Chrome's bootstrap writes the 0x42 bundle
                 * request immediately after subscribing, which transitions Waiting
                 * -> Streaming in app_handle_relay/app_start_stream — so any central
                 * still connected in Waiting past the deadline never requested the
                 * bundle and is safe to kick. The kick can never fire after 0x42:
                 * the screen is no longer Waiting. One kick per connection by
                 * design: the next rising edge re-stamps ble_connected_since.
                 * 15 s budget (not shorter): on a FRESH origin the first bootstrap
                 * Connect runs a pairing ceremony (numeric code shown on the
                 * Flipper + human reaction time) BEFORE 0x42 is written; hardware
                 * showed a 3 s window killing that first connect mid-pairing.
                 * 15 s covers the ceremony while still cycling squatters off the
                 * link often enough for pickers to catch advertising windows. */
                if(app->ble_connected &&
                   now - app->ble_connected_since >= BLE_WAITING_SQUATTER_KICK_MS) {
                    FURI_LOG_W(TAG, "BLE Waiting: kicking silent squatter");
                    bt_disconnect(app->bt);
                    furi_hal_bt_start_advertising();
                    /* Defer re-check while the async disconnect lands; the next
                     * connection's rising edge overwrites this with a fresh window. */
                    app->ble_connected_since = now;
                }
            }
        }
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
    app_restore_ble(app);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    storage_file_free(app->io_file);
    storage_file_free(app->stream_file);
    furi_record_close(RECORD_STORAGE);
    view_port_free(view_port);
    furi_message_queue_free(app->event_queue);
    free(app->bootstrap);
    free(app);

    return 0;
}
