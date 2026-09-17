#pragma once

#include "airbridge_app_i.h"

void airbridge_utilities_load(AirbridgeApp* app);
void airbridge_utilities_enter(void* context, AirbridgeScreen screen);
void airbridge_utilities_move(void* context, int direction);
bool airbridge_utilities_confirm(void* context, AirbridgeScreen screen);
void airbridge_utilities_mouse(AirbridgeApp* app);
void airbridge_utilities_snapshot(AirbridgeApp* app, AirbridgeUiSnapshot* snapshot);
