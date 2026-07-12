#pragma once

#include "drm_warpper.h"
#include <lvgl.h>
#include <stdbool.h>

typedef enum {
    MAINTENANCE_KEY_NONE = 0,
    MAINTENANCE_KEY_PREV,
    MAINTENANCE_KEY_NEXT,
    MAINTENANCE_KEY_ENTER,
    MAINTENANCE_KEY_BACK
} maintenance_key_t;

typedef struct {
    drm_warpper_t drm;
    buffer_object_t buffer;
    lv_display_t *display;
    uint8_t *draw_buffer;
    int input_fd;
    int width, height;
} maintenance_platform_t;

bool maintenance_platform_init(maintenance_platform_t *platform);
void maintenance_platform_destroy(maintenance_platform_t *platform);
maintenance_key_t maintenance_platform_read_key(maintenance_platform_t *platform);
void maintenance_platform_set_brightness(int level);
