#pragma once
#include "canvas.h"
#include "../input/input.h"
#include <stdbool.h>
typedef struct ViewPort ViewPort;
typedef void (*ViewPortDrawCallback)(Canvas*, void*);
typedef void (*ViewPortInputCallback)(InputEvent*, void*);
ViewPort* view_port_alloc(void);
void view_port_free(ViewPort* viewport);
void view_port_enabled_set(ViewPort* viewport, bool enabled);
void view_port_update(ViewPort* viewport);
void view_port_draw_callback_set(ViewPort* viewport, ViewPortDrawCallback callback, void* context);
void view_port_input_callback_set(
    ViewPort* viewport,
    ViewPortInputCallback callback,
    void* context);
