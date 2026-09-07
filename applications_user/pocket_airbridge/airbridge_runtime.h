#pragma once

#include <notification/notification_app.h>

#include "airbridge_app_i.h"
#include "airbridge_exit_contract.h"

const AirbridgeScreenActions* airbridge_runtime_screen_actions(void);
void airbridge_runtime_run(
    AirbridgeApp* app,
    NotificationApp* notifications,
    bool* startup_apply_pending,
    AirbridgeExitLatch* exit_latch);
