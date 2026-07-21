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

#include <bt/bt_service/bt_i.h>
#include <profiles/serial_profile.h>

#define TAG "AirBridge"

#define EVENT_TYPE_INPUT  (1 << 0)
#define EVENT_TYPE_USB    (1 << 1)
#define EVENT_TYPE_BLE    (1 << 2)
#define EVENT_TYPE_RELAY  (1 << 3)

#define MENU_ITEM_COUNT       3
#define BOOTSTRAP_MAX_BYTES   2048
#define TYPE_PRESS_DELAY_MS   12
#define TYPE_RELEASE_DELAY_MS 18
#define STREAM_TIMEOUT_MS     50

typedef enum {
    AirbridgeScreenMenu,
    AirbridgeScreenBridge,
    AirbridgeScreenDeployPrompt,
    AirbridgeScreenTyping,
    AirbridgeScreenWaiting,
    AirbridgeScreenStreaming,
    AirbridgeScreenDone,
    AirbridgeScreenError,
} AirbridgeScreen;

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
    FuriHalUsbInterface* usb_mode_prev;
    AirbridgeScreen screen;
    uint8_t menu_index;
    uint8_t profile_index;
    bool usb_configured;
    bool stream_open;
    bool stream_header_pending;
    uint32_t stream_total_len;
    uint32_t stream_checksum;
    uint32_t stream_sent;
    char bootstrap[BOOTSTRAP_MAX_BYTES + 1];
    size_t bootstrap_len;
    size_t typing_position;
    uint16_t typing_key;
    bool typing_key_down;
    bool typing_enter_pending;
    bool typing_enter_done;
    uint32_t typing_next_tick;
    char error[32];
} AirbridgeApp;

static uint32_t chunks_usb_to_ble;
static uint32_t chunks_ble_to_usb;
static uint32_t dropped;
static uint32_t tx_errors;
static bool usb_connected;
static bool usb_config_error;
static uint32_t last_heartbeat;

static void usb_event_callback(HidVendorEvent ev, void* context);
static uint16_t ble_raw_serial_callback(const uint8_t* data, uint16_t len, void* context);

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

static void app_abort_typing(AirbridgeApp* app) {
    if(app->typing_key_down) {
        furi_hal_hid_airbridge_kb_release_all();
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

static uint8_t app_load_profile(AirbridgeApp* app) {
    char config[64] = {0};
    if(!storage_file_open(app->io_file, APP_DATA_PATH("config"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        return FuriHalUsbAirbridgeProfileHpKbdVendor;
    }

    size_t read = storage_file_read(app->io_file, config, sizeof(config) - 1);
    storage_file_close(app->io_file);
    config[read] = '\0';
    const char* prefix = "profile=";
    if(strncmp(config, prefix, strlen(prefix)) != 0) {
        return FuriHalUsbAirbridgeProfileHpKbdVendor;
    }

    char* value = config + strlen(prefix);
    for(char* cursor = value; *cursor != '\0'; cursor++) {
        if(*cursor == '\r' || *cursor == '\n') {
            *cursor = '\0';
            break;
        }
    }
    return app_find_profile(value);
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
    bt_set_raw_serial_callback(ble_raw_serial_callback, app->event_queue);
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
    if(!storage_file_open(app->io_file, APP_DATA_PATH("bootstrap.js"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        app_show_error(app, "NO bootstrap.js ON SD");
        return false;
    }

    uint64_t file_size = storage_file_size(app->io_file);
    if(file_size == 0) {
        storage_file_close(app->io_file);
        app_show_error(app, "EMPTY bootstrap.js");
        return false;
    }
    if(file_size > BOOTSTRAP_MAX_BYTES) {
        storage_file_close(app->io_file);
        app_show_error(app, "bootstrap.js TOO LARGE");
        return false;
    }

    app->bootstrap_len = storage_file_read(app->io_file, app->bootstrap, file_size);
    storage_file_close(app->io_file);
    if(app->bootstrap_len != file_size) {
        app_show_error(app, "bootstrap.js READ ERROR");
        return false;
    }
    app->bootstrap[app->bootstrap_len] = '\0';

    return true;
}

static void app_start_typing(AirbridgeApp* app) {
    if(!app_load_bootstrap(app)) return;
    app->typing_position = 0;
    app->typing_key = HID_KEYBOARD_NONE;
    app->typing_key_down = false;
    app->typing_enter_pending = false;
    app->typing_enter_done = false;
    app->typing_next_tick = furi_get_tick();
    app->screen = AirbridgeScreenTyping;
}

static void app_typing_step(AirbridgeApp* app) {
    if(furi_get_tick() < app->typing_next_tick) return;

    if(app->typing_key_down) {
        if(!furi_hal_hid_airbridge_kb_release(app->typing_key)) {
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
        app->typing_next_tick = furi_get_tick() + TYPE_RELEASE_DELAY_MS;
        return;
    }

    if(app->typing_position < app->bootstrap_len) {
        uint16_t key = HID_ASCII_TO_KEY(app->bootstrap[app->typing_position]);
        if(key == HID_KEYBOARD_NONE) {
            app_show_error(app, "bootstrap.js NOT US ASCII");
            return;
        }
        if(!furi_hal_hid_airbridge_kb_press(key)) {
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key = key;
        app->typing_key_down = true;
        app->typing_next_tick = furi_get_tick() + TYPE_PRESS_DELAY_MS;
        return;
    }

    if(!app->typing_enter_done) {
        if(!furi_hal_hid_airbridge_kb_press(HID_KEYBOARD_RETURN)) {
            app_show_error(app, "KEYBOARD SEND ERROR");
            return;
        }
        app->typing_key = HID_KEYBOARD_RETURN;
        app->typing_key_down = true;
        app->typing_enter_pending = true;
        app->typing_next_tick = furi_get_tick() + TYPE_PRESS_DELAY_MS;
        return;
    }

    app->screen = AirbridgeScreenWaiting;
}

static bool app_start_stream(AirbridgeApp* app) {
    if(!storage_file_open(app->stream_file, APP_DATA_PATH("app-usb.html"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        app_show_error(app, "NO app-usb.html ON SD");
        return false;
    }
    app->stream_open = true;

    uint64_t file_size = storage_file_size(app->stream_file);
    if(file_size > UINT32_MAX) {
        app_stream_close(app);
        app_show_error(app, "app-usb.html TOO LARGE");
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
        app_show_error(app, "app-usb.html READ ERROR");
        return false;
    }

    app->stream_total_len = file_size;
    app->stream_checksum = checksum;
    app->stream_sent = 0;
    app->stream_header_pending = true;
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
    if(read != expected ||
       !furi_hal_hid_vendor_send_response_blocking(report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
        app_stream_close(app);
        app_show_error(app, "STREAM ERROR");
        return;
    }
    app->stream_sent += read;
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
    FuriMessageQueue* queue = context;
    if(len > 0 && len <= HID_VENDOR_PACKET_LEN) {
        BridgeEvent be = {
            .type = EVENT_TYPE_RELAY,
            .len = len,
            .to_ble = false,
        };
        memcpy(be.data, data, len);
        if(furi_message_queue_put(queue, &be, 0) == FuriStatusOk) {
            return HID_VENDOR_PACKET_LEN;
        }
        dropped++;
    }
    return 0;
}

static void draw_identity(Canvas* canvas, AirbridgeApp* app, uint8_t y) {
    const char* identity = furi_hal_usb_airbridge_profile_identity(app->profile_index);
    char line[32];
    snprintf(line, sizeof(line), "HID: %s", identity != NULL ? identity : "--");
    canvas_draw_str(canvas, 0, y, line);
}

static void render_menu(Canvas* canvas, AirbridgeApp* app) {
    const char* profile = furi_hal_usb_airbridge_profile_label(app->profile_index);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    canvas_draw_str(canvas, 10, 31, "Bridge");
    canvas_draw_str(canvas, 10, 42, "Deploy app");
    char line[32];
    snprintf(line, sizeof(line), "USB: %s", profile != NULL ? profile : "--");
    canvas_draw_str(canvas, 10, 53, line);
    canvas_draw_str(canvas, 0, 31 + (app->menu_index * 11), ">");
    canvas_draw_str(canvas, 0, 63, "BACK: Exit");
}

static void render_bridge(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);

    char line[32];
    snprintf(
        line,
        sizeof(line),
        "USB:%s BLE:%s",
        usb_connected ? "ON" : "--",
        furi_hal_bt_is_active() ? "ON" : "--");
    canvas_draw_str(canvas, 0, 28, line);
    if(usb_config_error) {
        canvas_draw_str(canvas, 80, 28, "ERR");
    }

    snprintf(line, sizeof(line), "U->B: %lu", chunks_usb_to_ble);
    canvas_draw_str(canvas, 0, 37, line);
    snprintf(line, sizeof(line), "B->U: %lu", chunks_ble_to_usb);
    canvas_draw_str(canvas, 0, 46, line);
    snprintf(line, sizeof(line), "DROP: %lu", dropped);
    canvas_draw_str(canvas, 0, 55, line);
    snprintf(line, sizeof(line), "TXERR: %lu", tx_errors);
    canvas_draw_str(canvas, 0, 63, line);
}

static void render_deploy_prompt(Canvas* canvas, AirbridgeApp* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Deploy app");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    canvas_draw_str(canvas, 0, 31, "Place cursor in browser");
    canvas_draw_str(canvas, 0, 42, "console, then press OK");
    canvas_draw_str(canvas, 0, 63, "BACK: Menu");
}

static void render_message(Canvas* canvas, AirbridgeApp* app, const char* title, const char* detail) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, title);
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, app, 19);
    canvas_draw_str(canvas, 0, 36, detail);
    canvas_draw_str(canvas, 0, 63, "BACK: Menu");
}

static void render_callback(Canvas* canvas, void* context) {
    AirbridgeApp* app = context;
    canvas_clear(canvas);
    switch(app->screen) {
    case AirbridgeScreenMenu:
        render_menu(canvas, app);
        break;
    case AirbridgeScreenBridge:
        render_bridge(canvas, app);
        break;
    case AirbridgeScreenDeployPrompt:
        render_deploy_prompt(canvas, app);
        break;
    case AirbridgeScreenTyping:
        render_message(canvas, app, "TYPING...", "BACK aborts");
        break;
    case AirbridgeScreenWaiting:
        render_message(canvas, app, "Waiting for request...", "Send bundle request");
        break;
    case AirbridgeScreenStreaming:
        render_message(canvas, app, "Serving app...", "BACK aborts");
        break;
    case AirbridgeScreenDone:
        render_message(canvas, app, "Done", "OK or BACK: Menu");
        break;
    case AirbridgeScreenError:
        render_message(canvas, app, "Deploy error", app->error);
        break;
    }
}

static void input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* queue = context;
    bool is_back_press =
        (input_event->key == InputKeyBack) && (input_event->type == InputTypePress);
    bool is_short_non_back =
        (input_event->key != InputKeyBack) && (input_event->type == InputTypeShort);
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
    if(app->screen == AirbridgeScreenMenu) {
        if(key == InputKeyBack) {
            *running = false;
        } else if(key == InputKeyUp) {
            app->menu_index = (app->menu_index + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        } else if(key == InputKeyDown) {
            app->menu_index = (app->menu_index + 1) % MENU_ITEM_COUNT;
        } else if(key == InputKeyOk) {
            if(app->menu_index == 0) {
                if(app_configure_usb(app, app->profile_index)) {
                    app->screen = AirbridgeScreenBridge;
                } else {
                    app_show_error(app, "USB CONFIG ERROR");
                }
            } else if(app->menu_index == 1) {
                if(!furi_hal_usb_airbridge_profile_has_keyboard(app->profile_index)) {
                    app_show_error(app, "Set USB to Kbd+Vendor");
                } else if(app_configure_usb(app, app->profile_index)) {
                    app->screen = AirbridgeScreenDeployPrompt;
                } else {
                    app_show_error(app, "USB CONFIG ERROR");
                }
            } else {
                app_show_error(app, "Set profile in config");
            }
        }
        return;
    }

    if(app->screen == AirbridgeScreenBridge) {
        if(key == InputKeyBack) {
            app->screen = AirbridgeScreenMenu;
        }
        return;
    }

    if(app->screen == AirbridgeScreenDeployPrompt) {
        if(key == InputKeyBack) {
            app->screen = AirbridgeScreenMenu;
        } else if(key == InputKeyOk) {
            app_start_typing(app);
        }
        return;
    }

    if(app->screen == AirbridgeScreenTyping) {
        if(key == InputKeyBack) {
            app_abort_typing(app);
            app->screen = AirbridgeScreenMenu;
        }
        return;
    }

    if(app->screen == AirbridgeScreenStreaming) {
        if(key == InputKeyBack) {
            app_stream_close(app);
            app->screen = AirbridgeScreenMenu;
        }
        return;
    }

    if(key == InputKeyBack || key == InputKeyOk) {
        app->screen = AirbridgeScreenMenu;
    }
}

static void app_handle_relay(AirbridgeApp* app, BridgeEvent* be) {
    if(app->screen == AirbridgeScreenBridge) {
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
    } else if(
        (app->screen == AirbridgeScreenWaiting) && be->to_ble && (be->len > 0) &&
        (be->data[0] == 0x42)) {
        app_start_stream(app);
    }
}

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);
    AirbridgeApp* app = malloc(sizeof(*app));
    if(app == NULL) return -1;
    memset(app, 0, sizeof(*app));
    app->screen = AirbridgeScreenMenu;
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

    app->profile_index = app_load_profile(app);
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

        // This first event-loop turn is deferred beyond loader open while minimizing CDC exposure.
        if(startup_apply_pending) {
            startup_apply_pending = false;
            if(!app_configure_usb(app, app->profile_index)) {
                app_show_error(app, "USB CONFIG ERROR");
            }
        }

        if(app->screen == AirbridgeScreenTyping) {
            app_typing_step(app);
        } else if(app->screen == AirbridgeScreenStreaming) {
            app_stream_step(app);
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
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    storage_file_free(app->io_file);
    storage_file_free(app->stream_file);
    furi_record_close(RECORD_STORAGE);
    view_port_free(view_port);
    furi_message_queue_free(app->event_queue);
    free(app);

    return 0;
}
