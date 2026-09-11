#pragma once

#include "flooper_player.h"
#include <furi.h>
#include <gui/canvas.h>

typedef enum {
    FlooperSplash,
    FlooperSelector,
    FlooperPlayback
} FlooperScreen;
typedef struct {
    FlooperScreen screen;
    FlooperSnapshot snapshot;
    char selected_name[FLOOPER_ID_BYTES];
    uint32_t minimum_generation, pose_tick;
    uint8_t last_label;
    bool contact;
} FlooperUiModel;
typedef struct {
    FuriMutex* mutex;
    FlooperUiModel model;
} FlooperUi;

/* Main owns model writes under mutex; draw copies before any Canvas operation. */
void flooper_ui_tick(FlooperUiModel* model, const FlooperSnapshot* snapshot, uint32_t tick);
void flooper_ui_draw(Canvas* canvas, void* context);
