/*
 * SUPERSEDED PSEUDOCODE — kept for historical reference only.
 *
 * The live FAP is applications_user/pocket_airbridge/pocket_airbridge.c
 * in the flipperzero-firmware repo (~/projects/flipperzero-firmware).
 * See docs/firmware-guide.md for the shipped design.
 *
 * Pocket AirBridge — Flipper Zero Bridge Application
 *
 * This is pseudocode/C skeleton for the Flipper Zero firmware app.
 * It bridges USB HID vendor-defined reports to BLE GATT notifications
 * and vice-versa, with single-chunk buffering.
 *
 * Build against the official Flipper Zero firmware SDK (furi_hal, bt, gui).
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <notification/notification_messages.h>
#include <bt/bt_service/bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>

/* ---------- Configuration ---------- */

#define AIRBRIDGE_VERSION     "0.1"
#define REPORT_SIZE           64
#define MAX_PAYLOAD           59
#define ACK_TIMEOUT_MS        5000
#define MAX_RETRIES           3

// USB HID: vendor-defined usage page 0xFF00, usage 0x01
#define HID_USAGE_PAGE_VENDOR 0xFF00
#define HID_USAGE_VENDOR      0x01

// BLE GATT UUIDs (must match web apps)
#define SVC_UUID             "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHAR_TX_UUID         "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // Bridge -> Receiver
#define CHAR_RX_UUID         "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // Receiver -> Bridge

/* ---------- Protocol Constants ---------- */

typedef enum {
    MSG_HELLO  = 0x01,
    MSG_META   = 0x02,
    MSG_DATA   = 0x03,
    MSG_ACK    = 0x04,
    MSG_NACK   = 0x05,
    MSG_DONE   = 0x06,
    MSG_ERROR  = 0x07,
} MessageType;

/* ---------- State ---------- */

typedef enum {
    STATE_IDLE,
    STATE_USB_READY,
    STATE_BLE_READY,
    STATE_BRIDGE_ACTIVE,
    STATE_WAITING_BLE_ACK,
    STATE_ERROR,
} BridgeState;

typedef struct {
    BridgeState state;

    bool usb_connected;
    bool ble_connected;

    uint8_t chunk_buffer[MAX_PAYLOAD];
    uint16_t chunk_len;
    uint16_t chunk_seq;

    uint16_t total_chunks;
    uint16_t chunks_forwarded;

    uint32_t session_id;

    FuriMutex* usb_mutex;
    FuriSemaphore* ble_ack_sem;

    Bt* bt;
    Gui* gui;
    ViewPort* view_port;
} AirBridgeApp;

/* ---------- Message Helpers ---------- */

static uint16_t get_u16be(const uint8_t* buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static void set_u16be(uint8_t* buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

/* Build a protocol message into a 64-byte HID report */
static void build_report(uint8_t* report, MessageType type, uint16_t seq, const uint8_t* payload, uint16_t len) {
    memset(report, 0, REPORT_SIZE);
    report[0] = (uint8_t)type;
    set_u16be(&report[1], seq);
    set_u16be(&report[3], len);
    if (payload && len > 0 && len <= MAX_PAYLOAD) {
        memcpy(&report[5], payload, len);
    }
}

/* Parse the first 5 bytes of a report */
static bool parse_header(const uint8_t* report, MessageType* out_type, uint16_t* out_seq, uint16_t* out_len) {
    *out_type = (MessageType)report[0];
    *out_seq  = get_u16be(&report[1]);
    *out_len  = get_u16be(&report[3]);
    return (*out_len <= MAX_PAYLOAD);
}

/* ---------- USB HID Vendor Interface ---------- */

/*
 * In the real firmware, you would register a custom HID report descriptor:
 *
 *   Usage Page (Vendor Defined 0xFF00)
 *   Usage (0x01)
 *   Collection (Application)
 *     Report Size (8)
 *     Report Count (64)
 *     Usage (0x01)
 *     Input (Data, Var, Abs)
 *     Usage (0x02)
 *     Output (Data, Var, Abs)
 *   End Collection
 *
 * The Flipper SDK's furi_hal_usb_hid allows custom descriptors via the hid_profile
 * mechanism, or you can implement a custom USB class.
 *
 * For this prototype, we assume two callbacks:
 *   - on_hid_report_received(const uint8_t* data, uint16_t len)
 *   - hid_send_report(const uint8_t* data, uint16_t len)
 */

static void on_hid_report_received(AirBridgeApp* app, const uint8_t* data, uint16_t len) {
    if (len < 5) return;

    MessageType type;
    uint16_t seq, payload_len;
    if (!parse_header(data, &type, &seq, &payload_len)) return;
    const uint8_t* payload = &data[5];

    furi_mutex_acquire(app->usb_mutex, FuriWaitForever);

    switch (type) {
        case MSG_HELLO:
        case META:
        case MSG_DATA:
        case MSG_DONE:
            /* Forward to BLE side immediately (or buffer DATA) */
            if (type == MSG_DATA) {
                /* Buffer one chunk */
                if (payload_len > MAX_PAYLOAD) {
                    app->state = STATE_ERROR;
                    break;
                }
                memcpy(app->chunk_buffer, payload, payload_len);
                app->chunk_len = payload_len;
                app->chunk_seq = seq;
                app->state = STATE_WAITING_BLE_ACK;
            }
            /* Send via BLE GATT notification/write */
            ble_gatt_notify(CHAR_TX_UUID, data, 5 + payload_len);
            break;

        case MSG_ERROR:
            /* Forward error to BLE */
            ble_gatt_notify(CHAR_TX_UUID, data, 5 + payload_len);
            app->state = STATE_ERROR;
            break;

        default:
            break;
    }

    furi_mutex_release(app->usb_mutex);
}

static void hid_send_to_host(AirBridgeApp* app, const uint8_t* data, uint16_t len) {
    /* Uses furi_hal_usb_hid_send_report() or custom USB IN endpoint */
    furi_hal_usb_hid_send_report(data, len);
}

/* ---------- BLE GATT ---------- */

/*
 * BLE setup uses the Flipper bt service.
 * We advertise as a peripheral with the custom service UUID.
 *
 * Two characteristics:
 *   TX (notify): bridge pushes data to receiver.
 *   RX (write + write-response): receiver pushes ACKs/NACKs to bridge.
 */

static void ble_on_rx(AirBridgeApp* app, const uint8_t* data, uint16_t len) {
    if (len < 5) return;

    MessageType type;
    uint16_t seq, payload_len;
    if (!parse_header(data, &type, &seq, &payload_len)) return;

    if (type == MSG_ACK || type == MSG_NACK) {
        const uint8_t* acked_seq_ptr = &data[5];
        uint16_t acked_seq = get_u16be(acked_seq_ptr);

        /* If we were waiting for ACK on a DATA chunk, validate it */
        if (app->state == STATE_WAITING_BLE_ACK) {
            if (acked_seq == app->chunk_seq) {
                app->chunks_forwarded++;
                app->chunk_len = 0;
                app->state = STATE_BRIDGE_ACTIVE;
                /* Release sender so it can send the next chunk */
                furi_semaphore_release(app->ble_ack_sem);
            }
        }

        /* Forward ACK/NACK back to USB host regardless */
        hid_send_to_host(app, data, len);
    } else if (type == MSG_ERROR) {
        hid_send_to_host(app, data, len);
        app->state = STATE_ERROR;
    }
}

static void ble_on_connected(AirBridgeApp* app, bool connected) {
    app->ble_connected = connected;
    if (connected && app->usb_connected) {
        app->state = STATE_BRIDGE_ACTIVE;
    } else {
        app->state = app->usb_connected ? STATE_USB_READY : STATE_IDLE;
    }
}

/* ---------- Status Screen (GUI) ---------- */

static void draw_callback(Canvas* canvas, void* ctx) {
    AirBridgeApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 12, "Pocket AirBridge");

    canvas_set_font(canvas, FontSecondary);

    /* USB status */
    const char* usb_str = app->usb_connected ? "USB: CONNECTED" : "USB: --";
    canvas_draw_str(canvas, 0, 28, usb_str);

    /* BLE status */
    const char* ble_str = app->ble_connected ? "BLE: CONNECTED" : "BLE: --";
    canvas_draw_str(canvas, 0, 40, ble_str);

    /* Transfer progress */
    if (app->total_chunks > 0) {
        char progress[32];
        snprintf(progress, sizeof(progress), "Chunks: %u/%u", app->chunks_forwarded, app->total_chunks);
        canvas_draw_str(canvas, 0, 52, progress);

        uint8_t bar_w = (app->chunks_forwarded * 64) / app->total_chunks;
        canvas_draw_box(canvas, 0, 56, bar_w, 6);
        canvas_draw_frame(canvas, 0, 56, 64, 6);
    } else {
        canvas_draw_str(canvas, 0, 52, "Idle");
    }

    /* State label */
    const char* state_str = "IDLE";
    switch (app->state) {
        case STATE_USB_READY:   state_str = "USB Ready"; break;
        case STATE_BLE_READY:   state_str = "BLE Ready"; break;
        case STATE_BRIDGE_ACTIVE: state_str = "Active"; break;
        case STATE_WAITING_BLE_ACK: state_str = "Wait ACK"; break;
        case STATE_ERROR:       state_str = "ERROR"; break;
        default: break;
    }
    canvas_draw_str(canvas, 0, 64, state_str);
}

/* ---------- Main Thread ---------- */

static int32_t airbridge_worker(void* ctx) {
    AirBridgeApp* app = ctx;

    /* Initialize custom USB HID vendor interface */
    furi_hal_usb_hid_set_profile( ... ); // Register custom descriptor
    app->usb_connected = true;
    app->state = STATE_USB_READY;

    /* Initialize BLE peripheral with custom GATT service */
    app->bt = furi_record_open(RECORD_BT);
    bt_profile_set_custom(app->bt, ...); // Configure GATT table
    bt_keys_storage_set_custom(app->bt, ...);
    bt_set_profile(app->bt, BtProfileCustom);
    app->ble_connected = false;

    while (1) {
        /* Process USB RX queue (pumped by ISR) */
        uint8_t report[REPORT_SIZE];
        if (usb_hid_receive(report, REPORT_SIZE)) {
            on_hid_report_received(app, report, REPORT_SIZE);
        }

        /* Process BLE RX queue */
        uint8_t ble_pkt[REPORT_SIZE];
        uint16_t ble_len;
        if (ble_gatt_read_rx(ble_pkt, &ble_len)) {
            ble_on_rx(app, ble_pkt, ble_len);
        }

        /* Update GUI viewport */
        view_port_update(app->view_port);

        furi_delay_ms(10);
    }

    return 0;
}

/* ---------- App Entry ---------- */

int32_t pocket_airbridge_app(void* p) {
    UNUSED(p);

    AirBridgeApp* app = malloc(sizeof(AirBridgeApp));
    memset(app, 0, sizeof(AirBridgeApp));
    app->usb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->ble_ack_sem = furi_semaphore_alloc(1, 0);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    FuriThread* worker = furi_thread_alloc();
    furi_thread_set_name(worker, "AirBridgeWorker");
    furi_thread_set_stack_size(worker, 2048);
    furi_thread_set_callback(worker, airbridge_worker);
    furi_thread_set_context(worker, app);
    furi_thread_start(worker);

    /* Run until back button pressed */
    while (1) {
        uint32_t event = furi_message_queue_get(...); // Input event
        if (event == InputKeyBack) break;
    }

    furi_thread_flags_set(furi_thread_get_id(worker), ThreadFlagStop);
    furi_thread_join(worker);
    furi_thread_free(worker);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_mutex_free(app->usb_mutex);
    furi_semaphore_free(app->ble_ack_sem);
    free(app);

    return 0;
}

/*
 * Integration Notes for Flipper Firmware Build:
 *
 * 1. Create an application manifest (application.fam) in applications_user/pocket_airbridge/:
 *    appid="pocket_airbridge", name="Pocket AirBridge", entry_point="pocket_airbridge_app"
 *
 * 2. Add the custom HID descriptor to your firmware build or use the existing
 *    furi_hal_usb_hid APIs if they support vendor pages.
 *
 * 3. The BLE GATT service must be registered via the bt_service custom profile API.
 *    The official firmware has limited GATT flexibility; you may need to patch
 *    the stack or use the existing UART-over-BLE profile as a starting point.
 *
 * 4. For the hackathon, a quicker path may be to repurpose the Flipper's
 *    existing "BadUSB" HID infrastructure but with a vendor usage page,
 *    and repurpose the existing BLE serial profile for data transport.
 */
