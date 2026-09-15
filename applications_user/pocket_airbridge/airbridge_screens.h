#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airbridge_types.h"
#include "airbridge_ui.h"

typedef struct AirbridgeScreens AirbridgeScreens;

typedef struct {
    bool (*typing_start)(void* context, AirbridgeTypingTransport transport);
    bool (*typing_abort)(void* context);
    bool (*stream_start)(void* context, AirbridgeStreamTransport transport);
    void (*stream_close)(void* context);
    void (*ble_reset)(void* context);
    bool (*ble_profile_installed)(void* context);
    bool (*deploy_supported)(void* context, AirbridgeTypingTransport transport);
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
void airbridge_screens_deploy_requested(
    AirbridgeScreens* screens,
    AirbridgeTypingTransport transport);
/* Transport armed by the last confirmed deploy prompt. The relay matches the
 * 0x42 deploy frame direction against it; the stream serves its bundle. */
AirbridgeTypingTransport airbridge_screens_armed_transport(const AirbridgeScreens* screens);
/* Transport to render: the carousel selection on DeployPrompt, the armed
 * transport on Typing/Waiting/Streaming. */
AirbridgeTypingTransport airbridge_screens_render_transport(const AirbridgeScreens* screens);
void airbridge_screens_stream_complete(AirbridgeScreens* screens);
/* BLE deploy bookkeeping: accepted latches on stream start and clears on every
 * fresh link; the pump tick stamps Waiting entry and each pump service. */
bool airbridge_screens_deploy_request_accepted(const AirbridgeScreens* screens);
void airbridge_screens_note_ble_reconnect(AirbridgeScreens* screens);
uint32_t airbridge_screens_waiting_pump_tick(const AirbridgeScreens* screens);
void airbridge_screens_waiting_pump_mark(AirbridgeScreens* screens, uint32_t tick);
