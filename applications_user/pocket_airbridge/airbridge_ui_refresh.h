#pragma once

#include <string.h>

#include "airbridge_ui.h"

static inline bool airbridge_ui_refresh_needed(
    const AirbridgeUiSnapshot* previous,
    const AirbridgeUiSnapshot* next,
    bool initialized,
    bool interval_elapsed) {
    if(!initialized || previous->screen != next->screen ||
       previous->deploy_transport != next->deploy_transport ||
       previous->metrics.usb_connected != next->metrics.usb_connected ||
       previous->ble_connected != next->ble_connected ||
       previous->identity_warning != next->identity_warning ||
       previous->usb_profile_index != next->usb_profile_index ||
       previous->typing_generation != next->typing_generation ||
       previous->typing_total != next->typing_total ||
       previous->stream_total != next->stream_total ||
       previous->mouse_enabled != next->mouse_enabled ||
       previous->password_selected != next->password_selected ||
       previous->password_count != next->password_count ||
       strcmp(previous->ble_name, next->ble_name) ||
       strcmp(previous->menu_status, next->menu_status) ||
       strcmp(previous->error.title, next->error.title) ||
       strcmp(previous->error.detail, next->error.detail) ||
       strcmp(previous->error.action, next->error.action))
        return true;
    for(size_t i = 0; i < 3; i++) {
        if(strcmp(previous->password_names[i], next->password_names[i])) return true;
    }
    return interval_elapsed &&
           (previous->metrics.chunks_usb_to_ble != next->metrics.chunks_usb_to_ble ||
            previous->metrics.chunks_ble_to_usb != next->metrics.chunks_ble_to_usb ||
            previous->metrics.dropped != next->metrics.dropped ||
            previous->metrics.tx_errors != next->metrics.tx_errors ||
            previous->typing_position != next->typing_position ||
            previous->stream_sent != next->stream_sent ||
            previous->mouse_sent != next->mouse_sent ||
            previous->mouse_failed != next->mouse_failed);
}
