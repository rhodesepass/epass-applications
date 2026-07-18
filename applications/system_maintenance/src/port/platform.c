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
    maintenance_platform_t *platform = lv_display_get_user_data(display);
    if(lv_display_flush_is_last(display)) {
        int idx = (pixels == platform->buffers[1].vaddr) ? 1 : 0;
        drm_warpper_mount_layer(&platform->drm, 2, 0, 0, &platform->buffers[idx]);
    }
    lv_display_flush_ready(display);
}

bool maintenance_platform_init(maintenance_platform_t *platform)
{
    memset(platform, 0, sizeof(*platform));
    platform->input_fd_count = 0;
    if(drm_warpper_init(&platform->drm) < 0) return false;
    platform->width = platform->drm.conn->modes[0].hdisplay;
    platform->height = platform->drm.conn->modes[0].vdisplay;
    if(drm_warpper_init_layer(&platform->drm, 2, platform->width, platform->height,
                              DRM_WARPPER_LAYER_MODE_RGB565) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, 2, &platform->buffers[0]) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, 2, &platform->buffers[1]) < 0 ||
       drm_warpper_mount_layer(&platform->drm, 2, 0, 0, &platform->buffers[0]) < 0) {
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
    return true;
fail:
    maintenance_platform_destroy(platform);
    return false;
}

maintenance_key_t maintenance_platform_read_key(maintenance_platform_t *platform)
{
    struct input_event event;
    for(int i = 0; i < platform->input_fd_count; i++) {
        while(read(platform->input_fds[i], &event, sizeof(event)) == sizeof(event)) {
            if(event.type != EV_KEY || event.value == 0) continue;
            switch(event.code) {
            case KEY_1: case KEY_UP: case KEY_LEFT: return MAINTENANCE_KEY_PREV;
            case KEY_2: case KEY_DOWN: case KEY_RIGHT: return MAINTENANCE_KEY_NEXT;
            case KEY_3: case KEY_ENTER: return MAINTENANCE_KEY_ENTER;
            case KEY_4: case KEY_ESC: case KEY_BACK: return MAINTENANCE_KEY_BACK;
            default: break;
            }
        }
    }
    return MAINTENANCE_KEY_NONE;
}

void maintenance_platform_set_brightness(int level)
{
    FILE *fp = fopen("/sys/class/backlight/backlight/brightness", "w");
    if(fp) { fprintf(fp, "%d\n", level); fclose(fp); }
}

void maintenance_platform_destroy(maintenance_platform_t *platform)
{
    epass_input_close(platform->input_fds, platform->input_fd_count);
    if(platform->display) lv_display_delete(platform->display);
    drm_warpper_free_buffer(&platform->drm, &platform->buffers[0]);
    drm_warpper_free_buffer(&platform->drm, &platform->buffers[1]);
    drm_warpper_destroy(&platform->drm);
    memset(platform, 0, sizeof(*platform));
    platform->input_fd_count = 0;
}
