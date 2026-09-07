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

#include "airbridge_ui_i.h"

#define TAG "AirBridge"

static AirbridgeUiIntent airbridge_ui_intent_for_key(InputKey key) {
    switch(key) {
    case InputKeyBack:
        return AirbridgeUiIntentBack;
    case InputKeyLeft:
        return AirbridgeUiIntentPrevious;
    case InputKeyRight:
        return AirbridgeUiIntentNext;
    case InputKeyDown:
        return AirbridgeUiIntentResetBle;
    case InputKeyOk:
        return AirbridgeUiIntentConfirm;
    case InputKeyUp:
    case InputKeyMAX:
        return AirbridgeUiIntentOther;
    }
    return AirbridgeUiIntentConfirm;
}

void airbridge_ui_input_callback(InputEvent* input_event, void* context) {
    AirbridgeUi* ui = context;
    AirbridgeUiSnapshot snapshot;
    airbridge_ui_snapshot_copy(ui, &snapshot);
    if(snapshot.closing || snapshot.operation.stalled) return;
    if(input_event->key == InputKeyBack && input_event->type == InputTypeLong) {
        ui->input_paused = ui->intent_callback(ui->intent_context, AirbridgeUiIntentExit);
        return;
    }

    const bool is_back = (input_event->key == InputKeyBack) &&
                         (input_event->type == InputTypePress);
    const bool is_short_non_back = (input_event->key != InputKeyBack) &&
                                   (input_event->type == InputTypeShort);
    if(!is_back && !is_short_non_back) return;

    BridgeEvent event = {
        .type = EVENT_TYPE_INPUT,
        .tick = furi_get_tick(),
        .sequence = ui->next_input_sequence++,
        .key = input_event->key,
        .input_type = input_event->type,
    };
    if(is_back) {
        FURI_LOG_D(TAG, "BACK enqueued %lu", furi_get_tick());
        if(furi_message_queue_put(ui->back_queue, &event, 0) != FuriStatusOk) {
            FURI_LOG_D(TAG, "BACK coalesced");
        }
    } else if(furi_message_queue_put(ui->input_queue, &event, 0) != FuriStatusOk) {
        (void)ui->intent_callback(ui->intent_context, AirbridgeUiIntentInputDropped);
    }
}

void airbridge_ui_service_input(AirbridgeUi* ui) {
    if(ui->input_paused) return;
    while(true) {
        if(!ui->have_back_head) {
            ui->have_back_head =
                (furi_message_queue_get(ui->back_queue, &ui->back_head, 0) == FuriStatusOk);
        }
        if(!ui->have_input_head) {
            ui->have_input_head =
                (furi_message_queue_get(ui->input_queue, &ui->input_head, 0) == FuriStatusOk);
        }
        if(!ui->have_back_head && !ui->have_input_head) return;

        const bool take_back = ui->have_back_head &&
                               (!ui->have_input_head || airbridge_ui_event_before_values(
                                                            ui->back_head.tick,
                                                            ui->back_head.sequence,
                                                            ui->input_head.tick,
                                                            ui->input_head.sequence));
        const BridgeEvent* event = take_back ? &ui->back_head : &ui->input_head;
        ui->input_paused =
            ui->intent_callback(ui->intent_context, airbridge_ui_intent_for_key(event->key));
        if(take_back) {
            ui->have_back_head = false;
            FURI_LOG_D(TAG, "BACK handled %lu", furi_get_tick());
        } else {
            ui->have_input_head = false;
        }
        if(ui->input_paused) return;
    }
}

#endif
