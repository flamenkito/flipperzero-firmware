#include "airbridge_ui_i.h"

#include <stdlib.h>

AirbridgeUi* airbridge_ui_alloc(AirbridgeUiIntentCallback intent_callback, void* context) {
    AirbridgeUi* ui = malloc(sizeof(*ui));
    if(ui == NULL) return NULL;
    memset(ui, 0, sizeof(*ui));
    ui->input_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    ui->back_queue = furi_message_queue_alloc(4, sizeof(BridgeEvent));
    ui->snapshot_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    ui->closing_frame = furi_semaphore_alloc(1, 0);
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

static void airbridge_ui_frame_committed(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context) {
    UNUSED(data);
    UNUSED(size);
    UNUSED(orientation);
    AirbridgeUi* ui = context;
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    const bool acknowledge = ui->closing_rendered && !ui->closing_committed;
    if(acknowledge) ui->closing_committed = true;
    furi_mutex_release(ui->snapshot_mutex);
    if(acknowledge) furi_semaphore_release(ui->closing_frame);
}

void airbridge_ui_add_view(AirbridgeUi* ui) {
    gui_add_framebuffer_callback(ui->gui, airbridge_ui_frame_committed, ui);
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
    snapshot->closing = ui->closing;
    snapshot->operation = ui->operation;
    furi_mutex_release(ui->snapshot_mutex);
}

void airbridge_ui_show_closing(AirbridgeUi* ui) {
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    ui->closing = true;
    furi_mutex_release(ui->snapshot_mutex);
    view_port_update(ui->view_port);
    /* The independent GUI callback acknowledges a committed Closing frame. */
    furi_check(furi_semaphore_acquire(ui->closing_frame, FuriWaitForever) == FuriStatusOk);
}

void airbridge_ui_closing_rendered(AirbridgeUi* ui) {
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    ui->closing_rendered = true;
    furi_mutex_release(ui->snapshot_mutex);
}

void airbridge_ui_set_operation_status(AirbridgeUi* ui, AirbridgeOperationStatus status) {
    furi_check(furi_mutex_acquire(ui->snapshot_mutex, FuriWaitForever) == FuriStatusOk);
    const bool changed = ui->operation.stalled != status.stalled ||
                         (ui->closing && ui->operation.operation != status.operation);
    ui->operation = status;
    furi_mutex_release(ui->snapshot_mutex);
    if(changed) view_port_update(ui->view_port);
}

void airbridge_ui_resume_input(AirbridgeUi* ui) {
    ui->input_paused = false;
}

void airbridge_ui_remove_view(AirbridgeUi* ui) {
    gui_remove_view_port(ui->gui, ui->view_port);
    gui_remove_framebuffer_callback(ui->gui, airbridge_ui_frame_committed, ui);
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
    furi_semaphore_free(ui->closing_frame);
    free(ui);
}
