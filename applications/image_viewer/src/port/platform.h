#pragma once

#include "hal_display.h"
#include "hal_input.h"
#include <lvgl.h>
#include <stdbool.h>

typedef enum {
    IV_KEY_NONE = 0,
    IV_KEY_PREV,
    IV_KEY_NEXT,
    IV_KEY_ENTER,
    IV_KEY_BACK
} iv_key_t;

typedef struct {
    hal_display_t drm;
    hal_buffer_t buffers[2];
    lv_display_t *display;
    hal_input_t input;
    int width, height;
} iv_platform_t;

bool iv_platform_init(iv_platform_t *platform);
void iv_platform_destroy(iv_platform_t *platform);
iv_key_t iv_platform_read_key(iv_platform_t *platform);
