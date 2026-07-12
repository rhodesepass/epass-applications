#pragma once

#include "port/platform.h"

typedef struct tutorial_ui tutorial_ui_t;

tutorial_ui_t *tutorial_ui_create(tutorial_platform_t *platform);
void tutorial_ui_handle_key(tutorial_ui_t *ui, tutorial_key_t key);
void tutorial_ui_tick(tutorial_ui_t *ui);
bool tutorial_ui_should_exit(const tutorial_ui_t *ui);
void tutorial_ui_destroy(tutorial_ui_t *ui);
