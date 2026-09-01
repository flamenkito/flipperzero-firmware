#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define JIGGLE_INTERVAL_MS 60000

// SIX Group logo - 50x20 1-bit XBM
#define SIX_LOGO_WIDTH  50
#define SIX_LOGO_HEIGHT 20
static const uint8_t six_logo_bits[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x1f, 0xe3, 0x07, 0xf8, 0x00, 0x00,
    0xc0, 0x1f, 0xe3, 0x0f, 0xfe, 0x00, 0x00, 0xe0, 0x00, 0x03, 0x1c, 0x07,
    0x00, 0x00, 0x70, 0x00, 0x03, 0x38, 0x03, 0x00, 0x00, 0x38, 0x00, 0x03,
    0xf0, 0x01, 0x00, 0x00, 0x1c, 0x00, 0x03, 0xe0, 0x01, 0x00, 0x00, 0x0e,
    0x00, 0x03, 0xe0, 0x01, 0x00, 0x00, 0x07, 0x00, 0x03, 0xf0, 0x01, 0x00,
    0x00, 0x03, 0x00, 0x03, 0x38, 0x03, 0x00, 0x80, 0x01, 0x00, 0x03, 0x1c,
    0x07, 0x00, 0xfc, 0x01, 0x00, 0xe3, 0x0f, 0xfe, 0x00, 0x7c, 0x00, 0x00,
    0xe3, 0x07, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define BACK_PRESS_TIMEOUT_MS 2000
#define INTERVAL_STEP_MS      1000
#define INTERVAL_MIN_MS       1000
#define INTERVAL_MAX_MS       60000

typedef struct {
    uint32_t interval_ms;
    uint8_t progress;
} MouseJigglerState;

typedef enum {
    EventTypeInput,
} EventType;

typedef struct {
    union {
        InputEvent input;
    };
    EventType type;
} MouseJigglerEvent;

static void render_callback(Canvas* canvas, void* ctx) {
    MouseJigglerState* state = ctx;
    canvas_clear(canvas);

    /* Draw SIX Group logo centered at top of screen */
    canvas_draw_xbm(
        canvas,
        (128 - SIX_LOGO_WIDTH) / 2,
        0,
        SIX_LOGO_WIDTH,
        SIX_LOGO_HEIGHT,
        six_logo_bits);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 28, "SIX Mouse");

    /* Show current interval */
    canvas_set_font(canvas, FontSecondary);
    char interval_str[20];
    snprintf(interval_str, sizeof(interval_str), "Interval: %lus", state->interval_ms / 1000);
    canvas_draw_str(canvas, 0, 40, interval_str);

    /* Progress bar frame */
    uint8_t bar_x = 14;
    uint8_t bar_y = 48;
    uint8_t bar_w = 100;
    uint8_t bar_h = 5;
    canvas_draw_frame(canvas, bar_x, bar_y, bar_w, bar_h);
    /* Progress bar fill */
    uint8_t fill_w = (uint8_t)((uint16_t)(bar_w - 2) * state->progress / 100);
    if(fill_w > 0) {
        canvas_draw_box(canvas, bar_x + 1, bar_y + 1, fill_w, bar_h - 2);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 63, "Press Back 2x to exit");
}

static void input_callback(InputEvent* input_event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    MouseJigglerEvent event = {.type = EventTypeInput, .input = *input_event};
    furi_message_queue_put(queue, &event, FuriWaitForever);
}

int32_t six_mouse_app(void* p) {
    UNUSED(p);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(MouseJigglerEvent));
    ViewPort* view_port = view_port_alloc();

    FuriHalUsbInterface* usb_mode_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();

    FuriHalUsbHidConfig hid_cfg = {
        .vid = 0x1EA7,
        .pid = 0x0064,
        .manuf = "",
        .product = "",
    };
    furi_check(furi_hal_usb_set_config(&usb_hid, &hid_cfg) == true);

    MouseJigglerState state = {
        .interval_ms = JIGGLE_INTERVAL_MS,
        .progress = 0,
    };

    view_port_draw_callback_set(view_port, render_callback, &state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);

    const int8_t jiggle_pattern[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    uint8_t jiggle_step = 0;
    uint32_t last_jiggle_tick = furi_get_tick();
    uint32_t last_back_press_tick = 0;
    MouseJigglerEvent event;

    while(1) {
        uint32_t now = furi_get_tick();
        uint32_t elapsed = now - last_jiggle_tick;

        /* Jiggle when interval elapses */
        if(elapsed >= state.interval_ms) {
            furi_hal_hid_mouse_move(
                jiggle_pattern[jiggle_step][0], jiggle_pattern[jiggle_step][1]);
            jiggle_step = (jiggle_step + 1) % 4;
            notification_message(notification, &sequence_blink_green_10);
            last_jiggle_tick = furi_get_tick();
            state.progress = 0;
        } else {
            state.progress = (uint8_t)(elapsed * 100 / state.interval_ms);
        }

        FuriStatus status = furi_message_queue_get(event_queue, &event, 250);

        if(status == FuriStatusOk && event.type == EventTypeInput) {
            if(event.input.key == InputKeyBack && event.input.type == InputTypePress) {
                uint32_t back_now = furi_get_tick();
                if(last_back_press_tick != 0 &&
                   back_now - last_back_press_tick < BACK_PRESS_TIMEOUT_MS) {
                    break; /* Double-press exits */
                }
                last_back_press_tick = back_now;
            } else if(event.input.key == InputKeyUp) {
                if(event.input.type == InputTypePress || event.input.type == InputTypeRepeat) {
                    if(state.interval_ms < INTERVAL_MAX_MS) {
                        state.interval_ms += INTERVAL_STEP_MS;
                        if(state.interval_ms > INTERVAL_MAX_MS) {
                            state.interval_ms = INTERVAL_MAX_MS;
                        }
                    }
                }
            } else if(event.input.key == InputKeyDown) {
                if(event.input.type == InputTypePress || event.input.type == InputTypeRepeat) {
                    if(state.interval_ms > INTERVAL_MIN_MS) {
                        state.interval_ms -= INTERVAL_STEP_MS;
                        if(state.interval_ms < INTERVAL_MIN_MS) {
                            state.interval_ms = INTERVAL_MIN_MS;
                        }
                    }
                }
            }
        }

        view_port_update(view_port);
    }

    furi_hal_usb_set_config(usb_mode_prev, NULL);

    furi_record_close(RECORD_NOTIFICATION);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);

    return 0;
}
