#pragma once

#include "driver/drm_warpper.h"
#include "epass_input.h"
#include <lvgl.h>
#include <stdbool.h>

typedef enum {
    VP_KEY_NONE = 0,
    VP_KEY_PREV,
    VP_KEY_NEXT,
    VP_KEY_ENTER,
    VP_KEY_BACK
} vp_key_t;

/* UI 要测长按/连按，所以上报按下与抬起(repeat 也算按下) */
typedef struct {
    vp_key_t key;
    bool pressed;
    bool repeat;
} vp_key_event_t;

typedef struct {
    drm_warpper_t drm;
    /* 播放条：屏幕底部一条独立 RGB565 小 plane(LVGL 只画这一条)。
     * RGB565 无 alpha，做成全屏会整屏盖死视频层，故取小条 + 显隐靠
     * mount/unmount */
    buffer_object_t bar_buffers[2];
    lv_display_t *display;
    int last_flushed;
    bool bar_visible;
    int input_fds[EPASS_INPUT_MAX_FDS];
    int input_fd_count;
    int width, height;          /* 整屏 */
    int bar_width, bar_height;
    int bar_y;
} vp_platform_t;

bool vp_platform_init(vp_platform_t *platform);
void vp_platform_destroy(vp_platform_t *platform);

/* 取一条按键事件；无事件返回 false。一次主循环内应循环取空。 */
bool vp_platform_read_key_event(vp_platform_t *platform, vp_key_event_t *ev);

void vp_platform_show_bar(vp_platform_t *platform, bool show);
