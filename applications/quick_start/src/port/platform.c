#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    tutorial_platform_t *platform = lv_display_get_user_data(display);
    if(lv_display_flush_is_last(display)) {
        int idx = (pixels == platform->buffers[1].vaddr) ? 1 : 0;
        drm_warpper_mount_layer(&platform->drm, DRM_WARPPER_LAYER_UI, 0, 0, &platform->buffers[idx]);
    }
    lv_display_flush_ready(display);
}

bool tutorial_platform_init(tutorial_platform_t *platform)
{
    memset(platform, 0, sizeof(*platform));
    platform->input_fd_count = 0;
    if(drm_warpper_init(&platform->drm) < 0) return false;
    platform->width = platform->drm.conn->modes[0].hdisplay;
    platform->height = platform->drm.conn->modes[0].vdisplay;
    if(drm_warpper_init_layer(&platform->drm, DRM_WARPPER_LAYER_UI, platform->width, platform->height,
                              DRM_WARPPER_LAYER_MODE_RGB565) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, DRM_WARPPER_LAYER_UI, &platform->buffers[0]) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, DRM_WARPPER_LAYER_UI, &platform->buffers[1]) < 0 ||
       drm_warpper_mount_layer(&platform->drm, DRM_WARPPER_LAYER_UI, 0, 0, &platform->buffers[0]) < 0) {
        goto fail;
    }
    // DIRECT 模式让 LVGL 直接画进两块显存,零拷贝翻页需要行距与画面宽度紧密对齐
    if(platform->buffers[0].pitch != (uint32_t)platform->width * 2) goto fail;
    lv_init();
    lv_tick_set_cb(tick_ms);
    platform->display = lv_display_create(platform->width, platform->height);
    if(!platform->display) goto fail;
    lv_display_set_color_format(platform->display, LV_COLOR_FORMAT_RGB565);
    size_t bytes = (size_t)platform->width * platform->height * 2;
    lv_display_set_user_data(platform->display, platform);
    lv_display_set_flush_cb(platform->display, flush_cb);
    lv_display_set_buffers(platform->display, platform->buffers[0].vaddr,
                           platform->buffers[1].vaddr, bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    platform->input_fd_count = epass_input_open_nav(platform->input_fds, EPASS_INPUT_MAX_FDS);
    if(platform->input_fd_count <= 0) goto fail;
    /* 遮罩层拿不到就算了, 只是没有过渡动画, 不该让整个应用起不来 */
    if(drm_warpper_layer_can_fade(&platform->drm, DRM_WARPPER_LAYER_OVERLAY) &&
       drm_warpper_allocate_buffer(&platform->drm, DRM_WARPPER_LAYER_OVERLAY,
                                   &platform->overlay_buf) == 0)
        platform->has_overlay = true;
    return true;
fail:
    tutorial_platform_destroy(platform);
    return false;
}

tutorial_key_t tutorial_platform_read_key(tutorial_platform_t *platform)
{
    struct input_event event;
    for(int i = 0; i < platform->input_fd_count; i++) {
        while(read(platform->input_fds[i], &event, sizeof(event)) == sizeof(event)) {
            if(event.type != EV_KEY || event.value == 0) continue;
            switch(event.code) {
            case KEY_1: case KEY_UP: case KEY_LEFT: return TUTORIAL_KEY_PREV;
            case KEY_2: case KEY_DOWN: case KEY_RIGHT: return TUTORIAL_KEY_NEXT;
            case KEY_3: case KEY_ENTER: return TUTORIAL_KEY_ENTER;
            case KEY_4: case KEY_ESC: case KEY_BACK: return TUTORIAL_KEY_BACK;
            default: break;
            }
        }
    }
    return TUTORIAL_KEY_NONE;
}

void tutorial_platform_overlay_fill(tutorial_platform_t *platform, uint16_t rgb565)
{
    if(!platform->has_overlay) return;
    uint16_t *px = (uint16_t *)platform->overlay_buf.vaddr;
    size_t count = (size_t)platform->overlay_buf.pitch / 2 * platform->overlay_buf.height;
    for(size_t i = 0; i < count; i++) px[i] = rgb565;
}

void tutorial_platform_overlay_fade(tutorial_platform_t *platform, int from, int to, int ms)
{
    if(!platform->has_overlay) return;
    /* 起手先按 from 挂上 (alpha 与 fb 同一次 commit), 免得以上一轮的 alpha 闪一帧 */
    if(drm_warpper_mount_layer_alpha(&platform->drm, DRM_WARPPER_LAYER_OVERLAY, 0, 0,
                                     &platform->overlay_buf, (uint8_t)from) < 0)
        return;
    const int step_ms = 16; /* 约 60Hz, 再密屏幕也跟不上 */
    int steps = ms / step_ms;
    if(steps < 1) steps = 1;
    for(int i = 1; i <= steps; i++) {
        nanosleep(&(struct timespec){ 0, step_ms * 1000000L }, NULL);
        drm_warpper_set_layer_alpha(&platform->drm, DRM_WARPPER_LAYER_OVERLAY,
                                    (uint8_t)(from + (to - from) * i / steps));
    }
    /* 全透明就别继续占着 DE 的合成带宽 */
    if(to == 0) drm_warpper_unmount_layer(&platform->drm, DRM_WARPPER_LAYER_OVERLAY);
}

void tutorial_platform_set_brightness(int level)
{
    FILE *fp = fopen("/sys/class/backlight/backlight/brightness", "w");
    if(fp) { fprintf(fp, "%d\n", level); fclose(fp); }
}

void tutorial_platform_destroy(tutorial_platform_t *platform)
{
    epass_input_close(platform->input_fds, platform->input_fd_count);
    if(platform->display) lv_display_delete(platform->display);
    if(platform->has_overlay)
        drm_warpper_free_buffer(&platform->drm, &platform->overlay_buf);
    drm_warpper_free_buffer(&platform->drm, &platform->buffers[0]);
    drm_warpper_free_buffer(&platform->drm, &platform->buffers[1]);
    drm_warpper_destroy(&platform->drm);
    memset(platform, 0, sizeof(*platform));
    platform->input_fd_count = 0;
}
