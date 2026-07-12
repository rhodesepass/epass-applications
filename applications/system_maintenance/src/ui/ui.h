#pragma once

#include "../port/platform.h"

#include <stdbool.h>

typedef struct maintenance_ui maintenance_ui_t;

maintenance_ui_t *maintenance_ui_create(maintenance_platform_t *platform);
void maintenance_ui_destroy(maintenance_ui_t *ui);
void maintenance_ui_handle_key(maintenance_ui_t *ui, maintenance_key_t key);
void maintenance_ui_tick(maintenance_ui_t *ui);
bool maintenance_ui_should_exit(const maintenance_ui_t *ui);
