#include "airbridge_time.h"

static bool airbridge_ui_event_before_values(
    uint32_t lhs_tick,
    uint32_t lhs_sequence,
    uint32_t rhs_tick,
    uint32_t rhs_sequence) {
    if(lhs_tick == rhs_tick) return airbridge_u32_before(lhs_sequence, rhs_sequence);
    return airbridge_u32_before(lhs_tick, rhs_tick);
}

#ifdef TEST_WRAP
#include <assert.h>
#include <stdint.h>

int main(void) {
    assert(airbridge_ui_event_before_values(7, UINT32_MAX - 1U, 7, 2));
    assert(!airbridge_ui_event_before_values(7, 2, 7, UINT32_MAX - 1U));
    assert(airbridge_ui_event_before_values(UINT32_MAX - 1U, 9, 2, 1));
    assert(!airbridge_ui_event_before_values(2, 1, UINT32_MAX - 1U, 9));

    const uint32_t base = UINT32_MAX - 10U;
    const uint32_t press_deadline = base + 12U;
    const uint32_t release_deadline = base + 18U;
    assert(!airbridge_tick_reached(base, press_deadline));
    assert(airbridge_tick_reached(press_deadline, press_deadline));
    assert(!airbridge_tick_reached(press_deadline, release_deadline));
    assert(airbridge_tick_reached(release_deadline, release_deadline));
    return 0;
}
#else

#include "airbridge_ui.h"

#include <stdio.h>

#include <furi_hal_bt.h>
#include <furi_hal_usb_airbridge.h>
#include <input/input.h>

#include <bt/bt_service/bt.h>

#define TAG "AirBridge"

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

static void draw_identity(Canvas* canvas, const AirbridgeUiSnapshot* snapshot, uint8_t y) {
    if(snapshot->identity_warning) {
        canvas_draw_str(canvas, 0, y, "WARN: BLE ID DEFAULT");
        return;
    }
    const char* identity =
        furi_hal_usb_airbridge_profile_identity(snapshot->usb_profile_index);
    char line[32];
    snprintf(line, sizeof(line), "HID: %s", identity != NULL ? identity : "--");
    canvas_draw_str(canvas, 0, y, line);
}

static void render_bridge(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Pocket AirBridge");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);

    // Icon header row (y=24): direction composites + trash + alert.
    // Glyphs encode live link state: filled = connected, outline = down.
    draw_usb_glyph(canvas, 0, 24, snapshot->metrics.usb_connected);
    canvas_draw_xbm(canvas, 9, 26, 5, 5, icon_arrow_r);
    draw_bt_glyph(canvas, 16, 24, snapshot->ble_connected);

    draw_bt_glyph(canvas, 32, 24, snapshot->ble_connected);
    canvas_draw_xbm(canvas, 39, 26, 5, 5, icon_arrow_r);
    draw_usb_glyph(canvas, 46, 24, snapshot->metrics.usb_connected);

    canvas_draw_xbm(canvas, 64, 24, 8, 8, icon_trash);
    canvas_draw_xbm(canvas, 96, 24, 9, 8, icon_alert);

    char line[16];
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.chunks_usb_to_ble);
    canvas_draw_str_aligned(canvas, 0, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.chunks_ble_to_usb);
    canvas_draw_str_aligned(canvas, 32, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.dropped);
    canvas_draw_str_aligned(canvas, 64, 35, AlignLeft, AlignTop, line);
    snprintf(line, sizeof(line), "%lu", snapshot->metrics.tx_errors);
    canvas_draw_str_aligned(canvas, 96, 35, AlignLeft, AlignTop, line);
}

static void render_deploy_prompt(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        snapshot->transport == AirbridgeTypingTransportUsb ? "USB Deploy" : "BLE Deploy");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);
    if(snapshot->transport == AirbridgeTypingTransportUsb) {
        canvas_draw_str(canvas, 0, 31, "Place cursor in browser");
        canvas_draw_str(canvas, 0, 42, "console, then press OK");
    } else {
        canvas_draw_str(canvas, 0, 31, "Pair in BT settings:");
        canvas_draw_str(canvas, 0, 42, snapshot->ble_name);
        canvas_draw_str(canvas, 0, 54, "then press OK");
    }
}

static void draw_progress_bar(Canvas* canvas, uint8_t y, uint64_t pos, uint64_t total) {
    canvas_draw_frame(canvas, 4, y, 120, 8);
    if(total == 0) return;
    uint32_t fill_w = (uint32_t)(118ULL * pos / total);
    if(fill_w > 118) fill_w = 118;
    canvas_draw_box(canvas, 5, y + 1, fill_w, 6);
}

static void render_typing(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        snapshot->transport == AirbridgeTypingTransportUsb ? "TYPING via USB" :
                                                             "TYPING via BLE");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);
    draw_progress_bar(canvas, 28, snapshot->typing_position, snapshot->typing_total);

    char line[32];
    snprintf(
        line,
        sizeof(line),
        "%lu/%lu chars",
        (unsigned long)snapshot->typing_position,
        (unsigned long)snapshot->typing_total);
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, line);
    uint32_t pct = snapshot->typing_total > 0 ?
                       (uint32_t)(100ULL * snapshot->typing_position / snapshot->typing_total) :
                       0;
    snprintf(line, sizeof(line), "%lu%%", pct);
    canvas_draw_str_aligned(canvas, 124, 45, AlignRight, AlignTop, line);

    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

static void render_streaming(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas,
        0,
        10,
        snapshot->transport == AirbridgeTypingTransportUsb ? "Serving app via USB" :
                                                             "Serving app via BLE");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);
    draw_progress_bar(canvas, 28, snapshot->stream_sent, snapshot->stream_total);

    char line[32];
    uint64_t sent_t = snapshot->stream_sent * 10ULL / 1024ULL;
    uint64_t total_t = snapshot->stream_total * 10ULL / 1024ULL;
    snprintf(
        line,
        sizeof(line),
        "%lu.%lu/%lu.%lu KB",
        (unsigned long)(sent_t / 10),
        (unsigned long)(sent_t % 10),
        (unsigned long)(total_t / 10),
        (unsigned long)(total_t % 10));
    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, line);
    uint32_t pct = snapshot->stream_total > 0 ?
                       (uint32_t)(100ULL * snapshot->stream_sent / snapshot->stream_total) :
                       0;
    snprintf(line, sizeof(line), "%lu%%", pct);
    canvas_draw_str_aligned(canvas, 124, 45, AlignRight, AlignTop, line);

    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

static void render_waiting(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Waiting for browser...");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);

    canvas_draw_frame(canvas, 4, 28, 120, 8);
    uint32_t phase = (furi_get_tick() / 50) % 64;
    uint32_t offset = phase < 32 ? phase : 63 - phase;
    canvas_draw_box(canvas, 5 + offset * (118 - 12) / 31, 29, 12, 6);

    canvas_draw_str_aligned(canvas, 0, 45, AlignLeft, AlignTop, "Click Connect in the browser");
    canvas_draw_str(canvas, 0, 63, "BACK: abort");
}

static void render_message(
    Canvas* canvas,
    const AirbridgeUiSnapshot* snapshot,
    const char* title,
    const char* detail,
    const char* action) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, title);
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);
    canvas_draw_str(canvas, 0, 35, detail);
    canvas_draw_str(canvas, 0, 47, action);
    canvas_draw_str(canvas, 0, 63, "BACK: Bridge");
}

static void render_fatal(Canvas* canvas, const AirbridgeUiSnapshot* snapshot) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Fatal startup error");
    canvas_set_font(canvas, FontSecondary);
    draw_identity(canvas, snapshot, 19);
    canvas_draw_str(canvas, 0, 36, snapshot->error.detail);
    canvas_draw_str(canvas, 0, 63, "Long BACK: exit");
}

static void render_callback(Canvas* canvas, void* context) {
    AirbridgeUi* ui = context;
    AirbridgeUiSnapshot snapshot;
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    snapshot = ui->snapshot;
    furi_mutex_release(ui->snapshot_mutex);
    canvas_clear(canvas);
    switch(snapshot.screen) {
    case AirbridgeScreenBridge:
        render_bridge(canvas, &snapshot);
        break;
    case AirbridgeScreenDeployPrompt:
        render_deploy_prompt(canvas, &snapshot);
        break;
    case AirbridgeScreenTyping:
        render_typing(canvas, &snapshot);
        break;
    case AirbridgeScreenWaiting:
        render_waiting(canvas, &snapshot);
        break;
    case AirbridgeScreenStreaming:
        render_streaming(canvas, &snapshot);
        break;
    case AirbridgeScreenDone:
        render_message(canvas, &snapshot, "Done", "Transfer complete", "Ready for another deploy");
        break;
    case AirbridgeScreenError:
        render_message(
            canvas,
            &snapshot,
            snapshot.error.title,
            snapshot.error.detail,
            snapshot.error.action);
        break;
    case AirbridgeScreenFatal:
        render_fatal(canvas, &snapshot);
        break;
    }
}

static void input_callback(InputEvent* input_event, void* context) {
    AirbridgeUi* ui = context;
    if(input_event->key == InputKeyBack && input_event->type == InputTypeLong) {
        *ui->exit_requested = true;
        airbridge_relay_wake(ui->relay);
        return;
    }

    bool is_back =
        (input_event->key == InputKeyBack) && (input_event->type == InputTypePress);
    bool is_short_non_back = (input_event->key != InputKeyBack) &&
                             (input_event->type == InputTypeShort);
    if(!is_back && !is_short_non_back) return;

    BridgeEvent be = {
        .type = EVENT_TYPE_INPUT,
        .tick = furi_get_tick(),
        .sequence = ui->next_input_sequence++,
        .key = input_event->key,
        .input_type = input_event->type,
    };
    if(is_back) {
        FURI_LOG_D(TAG, "BACK enqueued %lu", furi_get_tick());
        if(furi_message_queue_put(ui->back_queue, &be, 0) != FuriStatusOk) {
            /* BACK is never dropped: a full back_queue already holds pending
             * BACK presses with the identical screen-leaving effect, so the new
             * press coalesces into them without eviction and without dropped++. */
            FURI_LOG_D(TAG, "BACK coalesced");
        }
    } else if(furi_message_queue_put(ui->input_queue, &be, 0) != FuriStatusOk) {
        airbridge_relay_count_drop(ui->relay);
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

static void app_carousel_next(AirbridgeUi* ui, int dir) {
    int current = 0;
    if(*ui->screen == AirbridgeScreenDeployPrompt) {
        current = (ui->typing->transport == AirbridgeTypingTransportBle) ? 2 : 1;
    }
    const int count = (int)COUNT_OF(app_carousel_states);
    const int next = (current + dir + count) % count;
    const AirbridgeScreen next_screen = app_carousel_states[next].screen;
    const AirbridgeTypingTransport next_transport = app_carousel_states[next].transport;

    if(next_transport == AirbridgeTypingTransportUsb &&
       !furi_hal_usb_airbridge_profile_has_keyboard(ui->config->usb_profile_index)) {
        /* A Vendor-only USB profile cannot type; the USB deploy prompt is unreachable for this profile. */
        ui->show_error(ui->app, "Set USB to Kbd+Vendor");
        return;
    }

    *ui->screen = next_screen;
    ui->typing->transport = next_transport;
}

static void app_handle_input(AirbridgeUi* ui, InputKey key, InputType type, bool* running) {
    /* Long BACK exits from any screen. A held BACK enqueues its Press first,
     * so the screen's short-BACK semantics (prompt -> Bridge, typing -> abort)
     * run on the way out — harmless. */
    if(key == InputKeyBack && type == InputTypeLong) {
        *running = false;
        return;
    }

    if(*ui->screen == AirbridgeScreenFatal) return;

    if(*ui->screen == AirbridgeScreenBridge) {
        if(key == InputKeyLeft) {
            app_carousel_next(ui, -1);
        } else if(key == InputKeyRight) {
            app_carousel_next(ui, +1);
        } else if(key == InputKeyDown && ui->ble->ble_profile_installed) {
            /* Manual BLE reset (demo escape hatch): force-drop any held link
             * (squatter, zombie, desync) and re-advertise immediately. */
            FURI_LOG_W(TAG, "Manual BLE reset");
            airbridge_ble_force_reconnect(ui->ble);
        }
        /* Short BACK is a no-op on Bridge so carousel browsing can never
         * exit by accident. */
        return;
    }

    if(*ui->screen == AirbridgeScreenDeployPrompt) {
        if(key == InputKeyBack) {
            ui->typing->transport = AirbridgeTypingTransportNone;
            *ui->screen = AirbridgeScreenBridge;
        } else if(key == InputKeyOk) {
            airbridge_typing_start(ui->typing, ui->typing->transport);
        } else if(key == InputKeyLeft) {
            app_carousel_next(ui, -1);
        } else if(key == InputKeyRight) {
            app_carousel_next(ui, +1);
        }
        return;
    }

    if(*ui->screen == AirbridgeScreenTyping) {
        if(key == InputKeyBack) {
            if(airbridge_typing_abort(ui->typing)) {
                *ui->screen = AirbridgeScreenBridge;
            } else {
                ui->show_error(ui->app, "STUCK KEY - TAP A KEY");
            }
        }
        return;
    }

    if(*ui->screen == AirbridgeScreenStreaming) {
        if(key == InputKeyBack) {
            airbridge_stream_close(ui->stream);
            *ui->screen = AirbridgeScreenBridge;
        }
        return;
    }

    if(*ui->screen == AirbridgeScreenWaiting) {
        if(key == InputKeyBack) {
            *ui->screen = AirbridgeScreenBridge;
        }
        return;
    }

    if(key == InputKeyBack || key == InputKeyOk) {
        *ui->screen = AirbridgeScreenBridge;
    }
}

void airbridge_ui_init(
    AirbridgeUi* ui,
    bool* running,
    volatile bool* exit_requested,
    AirbridgeScreen* screen,
    AirbridgeConfig* config,
    AirbridgeRelay* relay,
    AirbridgeBle* ble,
    AirbridgeTyping* typing,
    AirbridgeStream* stream,
    AirbridgeError* error,
    AirbridgeUiShowError show_error,
    AirbridgeApp* app) {
    ui->input_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    ui->back_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    ui->running = running;
    ui->exit_requested = exit_requested;
    ui->screen = screen;
    ui->config = config;
    ui->relay = relay;
    ui->ble = ble;
    ui->typing = typing;
    ui->stream = stream;
    ui->error = error;
    ui->show_error = show_error;
    ui->app = app;
    ui->snapshot_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
}

void airbridge_ui_init_view(AirbridgeUi* ui) {
    ui->view_port = view_port_alloc();
    view_port_draw_callback_set(ui->view_port, render_callback, ui);
    view_port_input_callback_set(ui->view_port, input_callback, ui);
}

void airbridge_ui_open_gui(AirbridgeUi* ui) {
    ui->gui = furi_record_open(RECORD_GUI);
}

void airbridge_ui_add_view(AirbridgeUi* ui) {
    gui_add_view_port(ui->gui, ui->view_port, GuiLayerFullscreen);
}

void airbridge_ui_update(AirbridgeUi* ui) {
    AirbridgeUiSnapshot next = {
        .screen = *ui->screen,
        .transport = ui->typing->transport,
        .ble_connected = ui->ble->ble_connected,
        .identity_warning = ui->config->identity_warning,
        .usb_profile_index = ui->config->usb_profile_index,
        .typing_position = ui->typing->position,
        .typing_total = ui->typing->bootstrap_len,
        .stream_sent = ui->stream->sent,
        .stream_total = ui->stream->total_len,
        .error = *ui->error,
    };
    airbridge_relay_metrics_snapshot(ui->relay, &next.metrics);
    snprintf(next.ble_name, sizeof(next.ble_name), "%s", ui->config->ble_identity.device_name);
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    ui->snapshot = next;
    furi_mutex_release(ui->snapshot_mutex);
    view_port_update(ui->view_port);
}

/* Drains both input queues in temporal order: heads are merged by
 * (tick, sequence) captured at enqueue time — lower tick wins, ties broken by
 * the monotonic sequence, so same-tick presses keep their press order.
 * furi_message_queue has no peek, so each queue's head is cached in the app
 * struct between steps. BACK events ride their own queue, so relay flooding
 * and non-BACK bursts can never evict them. */
void airbridge_ui_service_input(AirbridgeUi* ui) {
    if(*ui->exit_requested) {
        *ui->running = false;
        return;
    }
    while(*ui->running) {
        if(!ui->have_back_head) {
            ui->have_back_head =
                (furi_message_queue_get(ui->back_queue, &ui->back_head, 0) == FuriStatusOk);
        }
        if(!ui->have_input_head) {
            ui->have_input_head =
                (furi_message_queue_get(ui->input_queue, &ui->input_head, 0) == FuriStatusOk);
        }
        if(!ui->have_back_head && !ui->have_input_head) return;

        bool take_back = ui->have_back_head &&
                         (!ui->have_input_head ||
                          airbridge_ui_event_before_values(
                              ui->back_head.tick,
                              ui->back_head.sequence,
                              ui->input_head.tick,
                              ui->input_head.sequence));
        InputKey key = take_back ? ui->back_head.key : ui->input_head.key;
        InputType input_type = take_back ? ui->back_head.input_type : ui->input_head.input_type;
        if(take_back) {
            ui->have_back_head = false;
        } else {
            ui->have_input_head = false;
        }

        app_handle_input(ui, key, input_type, ui->running);
        if(!*ui->running || *ui->exit_requested) {
            *ui->running = false;
            return;
        }
        if(take_back) {
            FURI_LOG_D(TAG, "BACK handled %lu", furi_get_tick());
        }
    }
}

void airbridge_ui_remove_view(AirbridgeUi* ui) {
    gui_remove_view_port(ui->gui, ui->view_port);
}

void airbridge_ui_close_gui(void) {
    furi_record_close(RECORD_GUI);
}

void airbridge_ui_free_view(AirbridgeUi* ui) {
    view_port_free(ui->view_port);
}

void airbridge_ui_deinit(AirbridgeUi* ui) {
    furi_message_queue_free(ui->input_queue);
    furi_message_queue_free(ui->back_queue);
    furi_mutex_free(ui->snapshot_mutex);
}

#endif
