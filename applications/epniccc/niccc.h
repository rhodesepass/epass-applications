#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epass_game.h"

bool niccc_init(const char *scene_path);
void niccc_draw_frame(game_framebuffer_t *framebuffer, int frame_idx);
void niccc_destroy(void);