#pragma once

#include "drm_warpper.h"
#include <lvgl.h>
#include <stdbool.h>

typedef enum {
    TUTORIAL_KEY_NONE = 0,
    TUTORIAL_KEY_PREV,
    TUTORIAL_KEY_NEXT,
    TUTORIAL_KEY_ENTER,
    TUTORIAL_KEY_BACK
} tutorial_key_t;

typedef struct {
    drm_warpper_t drm;
    buffer_object_t buffer;
    lv_display_t *display;
    uint8_t *draw_buffer;
    int input_fd;
    int width, height;
} tutorial_platform_t;

bool tutorial_platform_init(tutorial_platform_t *platform);
void tutorial_platform_destroy(tutorial_platform_t *platform);
tutorial_key_t tutorial_platform_read_key(tutorial_platform_t *platform);
void tutorial_platform_set_brightness(int level);
