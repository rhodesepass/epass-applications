#include "platform.h"
#include "config.h"

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
    vp_platform_t *platform = lv_display_get_user_data(display);
    if(lv_display_flush_is_last(display)) {
        int idx = (pixels == platform->bar_buffers[1].vaddr) ? 1 : 0;
        platform->last_flushed = idx;
        /* 同步 mount 与显示线程的每 vsync commit 由 commit_mutex 串行，
         * UI 条帧率低，不走 display_queue 免记账 */
        if(platform->bar_visible)
            drm_warpper_mount_layer(&platform->drm, DRM_WARPPER_LAYER_UI,
                                    0, platform->bar_y,
                                    &platform->bar_buffers[idx]);
    }
    lv_display_flush_ready(display);
}

bool vp_platform_init(vp_platform_t *platform)
{
    memset(platform, 0, sizeof(*platform));
    if(drm_warpper_init(&platform->drm) < 0) return false;
    platform->width = platform->drm.conn->modes[0].hdisplay;
    platform->height = platform->drm.conn->modes[0].vdisplay;

    platform->bar_width = platform->width;
    platform->bar_height = VP_BAR_HEIGHT_REF * platform->width / VP_BAR_WIDTH_REF;
    platform->bar_y = platform->height - platform->bar_height;

    /* 视频层的 display/free 队列在 init_layer 里创建，mediaplayer 依赖 */
    if(drm_warpper_init_layer(&platform->drm, DRM_WARPPER_LAYER_VIDEO,
                              platform->width, platform->height,
                              DRM_WARPPER_LAYER_MODE_MB32_NV12) < 0)
        goto fail;
    if(drm_warpper_init_layer(&platform->drm, DRM_WARPPER_LAYER_UI,
                              platform->bar_width, platform->bar_height,
                              DRM_WARPPER_LAYER_MODE_RGB565) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, DRM_WARPPER_LAYER_UI,
                                   &platform->bar_buffers[0]) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, DRM_WARPPER_LAYER_UI,
                                   &platform->bar_buffers[1]) < 0)
        goto fail;
    // DIRECT 模式让 LVGL 直接画进两块显存,零拷贝翻页需要行距与画面宽度紧密对齐
    if(platform->bar_buffers[0].pitch != (uint32_t)platform->bar_width * 2)
        goto fail;

    lv_init();
    lv_tick_set_cb(tick_ms);
    platform->display = lv_display_create(platform->bar_width, platform->bar_height);
    if(!platform->display) goto fail;
    lv_display_set_color_format(platform->display, LV_COLOR_FORMAT_RGB565);
    size_t bytes = (size_t)platform->bar_width * platform->bar_height * 2;
    lv_display_set_user_data(platform->display, platform);
    lv_display_set_flush_cb(platform->display, flush_cb);
    lv_display_set_buffers(platform->display, platform->bar_buffers[0].vaddr,
                           platform->bar_buffers[1].vaddr, bytes,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    platform->input_fd_count = epass_input_open_nav(platform->input_fds,
                                                    EPASS_INPUT_MAX_FDS);
    if(platform->input_fd_count <= 0) goto fail;
    return true;
fail:
    vp_platform_destroy(platform);
    return false;
}

bool vp_platform_read_key_event(vp_platform_t *platform, vp_key_event_t *ev)
{
    struct input_event event;
    for(int i = 0; i < platform->input_fd_count; i++) {
        while(read(platform->input_fds[i], &event, sizeof(event)) == sizeof(event)) {
            if(event.type != EV_KEY) continue;
            vp_key_t key;
            switch(event.code) {
            case KEY_1: case KEY_UP: case KEY_LEFT: key = VP_KEY_PREV; break;
            case KEY_2: case KEY_DOWN: case KEY_RIGHT: key = VP_KEY_NEXT; break;
            case KEY_3: case KEY_ENTER: key = VP_KEY_ENTER; break;
            case KEY_4: case KEY_ESC: case KEY_BACK: key = VP_KEY_BACK; break;
            default: continue;
            }
            ev->key = key;
            ev->pressed = event.value != 0;
            ev->repeat = event.value == 2;
            return true;
        }
    }
    return false;
}

void vp_platform_show_bar(vp_platform_t *platform, bool show)
{
    if(platform->bar_visible == show) return;
    platform->bar_visible = show;
    if(show) {
        drm_warpper_mount_layer(&platform->drm, DRM_WARPPER_LAYER_UI,
                                0, platform->bar_y,
                                &platform->bar_buffers[platform->last_flushed]);
    } else {
        drm_warpper_disable_layer_sync(&platform->drm, DRM_WARPPER_LAYER_UI);
    }
}

void vp_platform_destroy(vp_platform_t *platform)
{
    epass_input_close(platform->input_fds, platform->input_fd_count);
    if(platform->display) lv_display_delete(platform->display);
    if(platform->bar_buffers[0].vaddr)
        drm_warpper_free_buffer(&platform->drm, DRM_WARPPER_LAYER_UI,
                                &platform->bar_buffers[0]);
    if(platform->bar_buffers[1].vaddr)
        drm_warpper_free_buffer(&platform->drm, DRM_WARPPER_LAYER_UI,
                                &platform->bar_buffers[1]);
    drm_warpper_destroy(&platform->drm);
    memset(platform, 0, sizeof(*platform));
}
