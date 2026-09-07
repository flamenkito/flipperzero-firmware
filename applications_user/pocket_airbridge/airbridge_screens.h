#pragma once

#include <stdbool.h>

#include "airbridge_types.h"
#include "airbridge_ui.h"

typedef struct AirbridgeScreens AirbridgeScreens;

typedef struct {
    bool (*typing_start)(void* context);
    bool (*typing_abort)(void* context);
    bool (*stream_start)(void* context);
    void (*stream_close)(void* context);
    void (*ble_reset)(void* context);
    bool (*ble_profile_installed)(void* context);
    bool (*deploy_supported)(void* context);
    void (*exit)(void* context);
    void (*input_dropped)(void* context);
} AirbridgeScreenActions;

AirbridgeScreens* airbridge_screens_alloc(
    const AirbridgeScreenActions* actions,
    void* context);
void airbridge_screens_free(AirbridgeScreens* screens);
AirbridgeScreen airbridge_screens_current(const AirbridgeScreens* screens);
const AirbridgeError* airbridge_screens_error(const AirbridgeScreens* screens);
bool airbridge_screens_handle_ui_intent(void* context, AirbridgeUiIntent intent);
void airbridge_screens_show_error(void* context, const char* message);
void airbridge_screens_show_fatal(AirbridgeScreens* screens, const char* message);
void airbridge_screens_typing_complete(AirbridgeScreens* screens);
void airbridge_screens_deploy_requested(AirbridgeScreens* screens);
void airbridge_screens_stream_complete(AirbridgeScreens* screens);
