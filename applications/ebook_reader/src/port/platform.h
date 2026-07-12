#pragma once

#include "drm_warpper.h"
#include <lvgl.h>
#include <stdbool.h>

typedef enum {
    EBOOK_KEY_NONE = 0,
    EBOOK_KEY_PREV,
    EBOOK_KEY_NEXT,
    EBOOK_KEY_ENTER,
    EBOOK_KEY_BACK
} ebook_key_t;

typedef struct {
    drm_warpper_t drm;
    buffer_object_t buffer;
    lv_display_t *display;
    uint8_t *draw_buffer;
    int input_fd;
    int width, height;
} ebook_platform_t;

bool ebook_platform_init(ebook_platform_t *platform);
void ebook_platform_destroy(ebook_platform_t *platform);
ebook_key_t ebook_platform_read_key(ebook_platform_t *platform);
void ebook_platform_set_brightness(int level);
