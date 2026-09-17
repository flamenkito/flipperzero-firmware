#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airbridge_relay.h"
#include "airbridge_types.h"
#include "airbridge_identity_params.h"
#include "airbridge_operation.h"

typedef struct AirbridgeUi AirbridgeUi;

typedef enum {
    AirbridgeUiIntentExit,
    AirbridgeUiIntentBack,
    AirbridgeUiIntentPrevious,
    AirbridgeUiIntentNext,
    AirbridgeUiIntentConfirm,
    AirbridgeUiIntentResetBle,
    AirbridgeUiIntentUp,
    AirbridgeUiIntentOther,
    AirbridgeUiIntentInputDropped,
} AirbridgeUiIntent;

typedef bool (*AirbridgeUiIntentCallback)(void* context, AirbridgeUiIntent intent);

typedef struct {
    AirbridgeScreen screen;
    AirbridgeTypingTransport deploy_transport;
    AirbridgeRelayMetrics metrics;
    bool ble_connected;
    bool identity_warning;
    uint8_t usb_profile_index;
    char ble_name[AIRBRIDGE_BLE_DEVICE_NAME_MAX_LEN + 1U];
    uint64_t typing_position;
    uint64_t typing_total;
    uint32_t typing_generation;
    uint64_t stream_sent;
    uint64_t stream_total;
    AirbridgeError error;
    bool closing;
    AirbridgeOperationStatus operation;
    bool mouse_enabled;
    uint32_t mouse_sent;
    uint32_t mouse_failed;
    uint8_t password_selected;
    uint8_t password_count;
    char password_names[3][25];
    char menu_status[32];
} AirbridgeUiSnapshot;

AirbridgeUi* airbridge_ui_alloc(AirbridgeUiIntentCallback intent_callback, void* context);
void airbridge_ui_init_view(AirbridgeUi* ui);
void airbridge_ui_open_gui(AirbridgeUi* ui);
void airbridge_ui_add_view(AirbridgeUi* ui);
void airbridge_ui_update(AirbridgeUi* ui, const AirbridgeUiSnapshot* snapshot);
bool airbridge_ui_password_ready(AirbridgeUi* ui, uint32_t generation);
void airbridge_ui_resume_input(AirbridgeUi* ui);
void airbridge_ui_service_input(AirbridgeUi* ui);
void airbridge_ui_show_closing(AirbridgeUi* ui);
void airbridge_ui_set_operation_status(AirbridgeUi* ui, AirbridgeOperationStatus status);
void airbridge_ui_remove_view(AirbridgeUi* ui);
void airbridge_ui_close_gui(void);
void airbridge_ui_free_view(AirbridgeUi* ui);
void airbridge_ui_free(AirbridgeUi* ui);
