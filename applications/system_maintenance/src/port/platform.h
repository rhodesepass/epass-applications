#pragma once

#include "drm_warpper.h"
#include "epass_input.h"
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
    buffer_object_t buffers[2];
    lv_display_t *display;
    int input_fds[EPASS_INPUT_MAX_FDS];
    int input_fd_count;
    int width, height;
} maintenance_platform_t;

bool maintenance_platform_init(maintenance_platform_t *platform);
void maintenance_platform_destroy(maintenance_platform_t *platform);
maintenance_key_t maintenance_platform_read_key(maintenance_platform_t *platform);
void maintenance_platform_set_brightness(int level);
