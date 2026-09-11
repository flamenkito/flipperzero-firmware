#pragma once
#include "view_port.h"
#define RECORD_GUI "gui"
typedef struct Gui Gui;
typedef enum {
    GuiLayerFullscreen
} GuiLayer;
void gui_add_view_port(Gui* gui, ViewPort* viewport, GuiLayer layer);
void gui_remove_view_port(Gui* gui, ViewPort* viewport);
