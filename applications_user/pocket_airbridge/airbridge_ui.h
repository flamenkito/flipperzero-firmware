#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airbridge_relay.h"
#include "airbridge_types.h"

typedef struct AirbridgeUi AirbridgeUi;

typedef enum {
    AirbridgeUiIntentExit,
    AirbridgeUiIntentBack,
    AirbridgeUiIntentPrevious,
    AirbridgeUiIntentNext,
    AirbridgeUiIntentConfirm,
    AirbridgeUiIntentResetBle,
    AirbridgeUiIntentOther,
    AirbridgeUiIntentInputDropped,
} AirbridgeUiIntent;

typedef bool (*AirbridgeUiIntentCallback)(void* context, AirbridgeUiIntent intent);

typedef struct {
    AirbridgeScreen screen;
    AirbridgeRelayMetrics metrics;
    bool ble_connected;
    bool identity_warning;
    uint8_t usb_profile_index;
    uint64_t typing_position;
    uint64_t typing_total;
    uint64_t stream_sent;
    uint64_t stream_total;
    AirbridgeError error;
} AirbridgeUiSnapshot;

AirbridgeUi* airbridge_ui_alloc(AirbridgeUiIntentCallback intent_callback, void* context);
void airbridge_ui_init_view(AirbridgeUi* ui);
void airbridge_ui_open_gui(AirbridgeUi* ui);
void airbridge_ui_add_view(AirbridgeUi* ui);
void airbridge_ui_update(AirbridgeUi* ui, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_resume_input(AirbridgeUi* ui);
void airbridge_ui_service_input(AirbridgeUi* ui);
void airbridge_ui_remove_view(AirbridgeUi* ui);
void airbridge_ui_close_gui(void);
void airbridge_ui_free_view(AirbridgeUi* ui);
void airbridge_ui_free(AirbridgeUi* ui);
