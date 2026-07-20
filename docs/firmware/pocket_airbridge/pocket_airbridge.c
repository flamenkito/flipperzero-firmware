#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_airbridge.h>
#include <bt/bt_service/bt_i.h>
#include <profiles/serial_profile.h>

#define TAG "AirBridge"

#define EVENT_TYPE_INPUT  (1 << 0)
#define EVENT_TYPE_USB    (1 << 1)
#define EVENT_TYPE_BLE    (1 << 2)
#define EVENT_TYPE_RELAY  (1 << 3)

typedef struct {
    uint32_t type;
    uint8_t data[HID_VENDOR_PACKET_LEN];
    uint16_t len;
    bool to_ble; // true = USB→BLE, false = BLE→USB
} BridgeEvent;

static uint32_t chunks_usb_to_ble = 0;
static uint32_t chunks_ble_to_usb = 0;
static uint32_t dropped = 0;
static uint32_t tx_errors = 0;
static bool usb_connected = false;
static bool usb_config_error = false;
static uint32_t last_heartbeat = 0;

/* ---------- USB Vendor HID Callbacks ---------- */

/* USB callbacks use timeout 0 because their callback context must not block. */
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

/* ---------- BLE Raw Serial Callback ---------- */

/* GAP callback uses timeout 0 because its callback context must not block. */
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

/* ---------- GUI ---------- */

static void render_callback(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");

    canvas_set_font(canvas, FontSecondary);
    char line[48];

    snprintf(line, sizeof(line), "USB: %s", usb_connected ? "CONNECTED" : "--");
    canvas_draw_str(canvas, 0, 19, line);
    if(usb_config_error) {
        canvas_draw_str(canvas, 54, 19, "ERR: CONFIG");
    }

    bool ble_active = furi_hal_bt_is_active();
    snprintf(line, sizeof(line), "BLE: %s", ble_active ? "ACTIVE" : "--");
    canvas_draw_str(canvas, 0, 28, line);

    snprintf(line, sizeof(line), "U->B: %lu", chunks_usb_to_ble);
    canvas_draw_str(canvas, 0, 37, line);

    snprintf(line, sizeof(line), "B->U: %lu", chunks_ble_to_usb);
    canvas_draw_str(canvas, 0, 46, line);

    snprintf(line, sizeof(line), "DROP: %lu", dropped);
    canvas_draw_str(canvas, 0, 55, line);

    snprintf(line, sizeof(line), "TXERR: %lu", tx_errors);
    canvas_draw_str(canvas, 0, 63, line);
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    BridgeEvent be = {.type = EVENT_TYPE_INPUT};
    if(input_event->type == InputTypeShort && input_event->key == InputKeyBack) {
        furi_message_queue_put(queue, &be, 0);
    }
}

/* ---------- Main App ---------- */

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(BridgeEvent));
    ViewPort* view_port = view_port_alloc();

    view_port_draw_callback_set(view_port, render_callback, NULL);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    /* Take over USB */
    FuriHalUsbInterface* usb_mode_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    bool usb_configured = furi_hal_usb_set_config(&usb_airbridge, NULL);
    if(usb_configured) {
        furi_hal_hid_vendor_set_callback(usb_event_callback, event_queue);
        bt_set_raw_serial_callback(ble_raw_serial_callback, event_queue);
    } else {
        usb_config_error = true;
    }

    bool running = true;
    while(running) {
        BridgeEvent be;
        FuriStatus status = furi_message_queue_get(event_queue, &be, 100);
        if(status == FuriStatusOk) {
            if(be.type == EVENT_TYPE_RELAY) {
                if(be.to_ble) {
                    chunks_usb_to_ble++;
                    if(!bt_serial_tx(be.data, be.len)) {
                        tx_errors++;
                    }
                } else {
                    /* Host HID drivers expect full 64-byte reports; pad with zeros. */
                    uint8_t report[HID_VENDOR_PACKET_LEN] = {0};
                    memcpy(report, be.data, be.len);
                    /* HAL API takes non-const; buffer is not modified */
                    furi_hal_hid_vendor_send_response(report, HID_VENDOR_PACKET_LEN);
                    chunks_ble_to_usb++;
                }
            } else if(be.type == EVENT_TYPE_USB) {
                usb_connected = be.to_ble;
            } else if(be.type == EVENT_TYPE_INPUT) {
                running = false;
            }
        }
        view_port_update(view_port);

        if(furi_get_tick() - last_heartbeat >= 500) {
            notification_message(notifications, &sequence_blink_green_100);
            last_heartbeat = furi_get_tick();
        }
    }

    /* Restore */
    if(usb_configured) {
        bt_set_raw_serial_callback(NULL, NULL);
        furi_hal_hid_vendor_set_callback(NULL, NULL);
    }
    furi_hal_usb_set_config(usb_mode_prev, NULL);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    return 0;
}
