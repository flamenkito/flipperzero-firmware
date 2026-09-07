#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#include "airbridge_ui.h"

struct AirbridgeUi {
    FuriMessageQueue* input_queue;
    FuriMessageQueue* back_queue;
    uint32_t next_input_sequence;
    bool have_back_head;
    BridgeEvent back_head;
    bool have_input_head;
    BridgeEvent input_head;
    ViewPort* view_port;
    Gui* gui;
    FuriMutex* snapshot_mutex;
    AirbridgeUiSnapshot snapshot;
    AirbridgeUiIntentCallback intent_callback;
    void* intent_context;
    bool input_paused;
    bool closing;
    bool closing_rendered;
    bool closing_committed;
    FuriSemaphore* closing_frame;
    AirbridgeOperationStatus operation;
};

void airbridge_ui_snapshot_copy(AirbridgeUi* ui, AirbridgeUiSnapshot* snapshot);
void airbridge_ui_input_callback(InputEvent* input_event, void* context);
void airbridge_ui_render_callback(Canvas* canvas, void* context);
void airbridge_ui_closing_rendered(AirbridgeUi* ui);
void airbridge_ui_render_bridge(Canvas* canvas, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_render_deploy_prompt(Canvas* canvas, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_render_typing(Canvas* canvas, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_render_waiting(Canvas* canvas, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_render_streaming(Canvas* canvas, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_render_message(
    Canvas* canvas,
    const AirbridgeUiSnapshot* snapshot,
    const char* title,
    const char* detail,
    const char* action);
void airbridge_ui_render_fatal(Canvas* canvas, const AirbridgeUiSnapshot* snapshot);
void airbridge_ui_draw_identity(Canvas* canvas, const AirbridgeUiSnapshot* snapshot, uint8_t y);
void airbridge_ui_draw_progress(Canvas* canvas, uint8_t y, uint64_t position, uint64_t total);
