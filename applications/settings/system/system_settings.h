#pragma once

#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <cli/cli_vcp.h>

typedef struct {
    Gui* gui;
    CliVcp* cli_vcp;
    ViewDispatcher* view_dispatcher;
    VariableItemList* var_item_list;
} SystemSettings;

typedef enum {
    SystemSettingsViewVarItemList,
} SystemSettingsView;
