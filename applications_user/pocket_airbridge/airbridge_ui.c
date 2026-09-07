#include "airbridge_ui_i.h"

#include <stdlib.h>

AirbridgeUi* airbridge_ui_alloc(AirbridgeUiIntentCallback intent_callback, void* context) {
    AirbridgeUi* ui = malloc(sizeof(*ui));
    if(ui == NULL) return NULL;
    memset(ui, 0, sizeof(*ui));
    ui->input_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    ui->back_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    ui->snapshot_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    ui->intent_callback = intent_callback;
    ui->intent_context = context;
    return ui;
}

void airbridge_ui_init_view(AirbridgeUi* ui) {
    ui->view_port = view_port_alloc();
    view_port_draw_callback_set(ui->view_port, airbridge_ui_render_callback, ui);
    view_port_input_callback_set(ui->view_port, airbridge_ui_input_callback, ui);
}

void airbridge_ui_open_gui(AirbridgeUi* ui) {
    ui->gui = furi_record_open(RECORD_GUI);
}

void airbridge_ui_add_view(AirbridgeUi* ui) {
    gui_add_view_port(ui->gui, ui->view_port, GuiLayerFullscreen);
}

void airbridge_ui_update(AirbridgeUi* ui, const AirbridgeUiSnapshot* snapshot) {
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    ui->snapshot = *snapshot;
    furi_mutex_release(ui->snapshot_mutex);
    view_port_update(ui->view_port);
}

void airbridge_ui_snapshot_copy(AirbridgeUi* ui, AirbridgeUiSnapshot* snapshot) {
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    *snapshot = ui->snapshot;
    furi_mutex_release(ui->snapshot_mutex);
}

void airbridge_ui_resume_input(AirbridgeUi* ui) {
    ui->input_paused = false;
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

void airbridge_ui_free(AirbridgeUi* ui) {
    furi_message_queue_free(ui->input_queue);
    furi_message_queue_free(ui->back_queue);
    furi_mutex_free(ui->snapshot_mutex);
    free(ui);
}
