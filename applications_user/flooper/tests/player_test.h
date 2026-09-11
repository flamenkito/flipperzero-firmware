#pragma once
#include "player_fake.h"
#include "../flooper_player.h"

extern const char player_document[];
FlooperPlayer* player_ready(void);
FlooperSnapshot player_snapshot(FlooperPlayer* player);
void player_command(FlooperPlayer* player, FlooperCommandType type);
uint64_t player_tick(uint64_t microseconds);
