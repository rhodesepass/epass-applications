#pragma once

#include "hal_display.h"
#include "epass_input.h"
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
    hal_display_t drm;
    hal_buffer_t buffers[2];
    lv_display_t *display;
    int input_fds[EPASS_INPUT_MAX_FDS];
    int input_fd_count;
    int width, height;
    /* 翻页过渡用的遮罩层。拿不到第二个 plane 或它不支持 alpha 时为 false,
     * 此时所有 overlay 调用退化成空操作 (host 预览工具也走这条路) */
    hal_buffer_t overlay_buf;
    bool has_overlay;
} tutorial_platform_t;

bool tutorial_platform_init(tutorial_platform_t *platform);
void tutorial_platform_destroy(tutorial_platform_t *platform);
tutorial_key_t tutorial_platform_read_key(tutorial_platform_t *platform);
void tutorial_platform_set_brightness(int level);

/* 遮罩层填成纯色 (RGB565) */
void tutorial_platform_overlay_fill(tutorial_platform_t *platform, uint16_t rgb565);
/* alpha 从 from 渐变到 to (0..255), 同步阻塞约 ms 毫秒。to==0 时收尾自动卸载图层。
 * 调用者须保证此间不碰 lv_timer_handler —— DRM commit 全在主线程, 借此免掉锁 */
void tutorial_platform_overlay_fade(tutorial_platform_t *platform, int from, int to, int ms);
