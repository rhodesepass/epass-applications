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
    ebook_platform_t *platform = lv_display_get_user_data(display);
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    for(int row = 0; row < height; row++) {
        uint8_t *dest = platform->buffer.vaddr +
            (size_t)(area->y1 + row) * platform->buffer.pitch + (size_t)area->x1 * 2;
        memcpy(dest, pixels + (size_t)row * width * 2, (size_t)width * 2);
    }
    lv_display_flush_ready(display);
}

bool ebook_platform_init(ebook_platform_t *platform)
{
    memset(platform, 0, sizeof(*platform));
    platform->input_fd = -1;
    if(drm_warpper_init(&platform->drm) < 0) return false;
    platform->width = platform->drm.conn->modes[0].hdisplay;
    platform->height = platform->drm.conn->modes[0].vdisplay;
    if(drm_warpper_init_layer(&platform->drm, 2, platform->width, platform->height,
                              DRM_WARPPER_LAYER_MODE_RGB565) < 0 ||
       drm_warpper_allocate_buffer(&platform->drm, 2, &platform->buffer) < 0 ||
       drm_warpper_mount_layer(&platform->drm, 2, 0, 0, &platform->buffer) < 0) {
        drm_warpper_destroy(&platform->drm);
        return false;
    }
    lv_init();
    lv_tick_set_cb(tick_ms);
    platform->display = lv_display_create(platform->width, platform->height);
    if(!platform->display) goto fail;
    lv_display_set_color_format(platform->display, LV_COLOR_FORMAT_RGB565);
    size_t rows = (size_t)(platform->height < 80 ? platform->height : 80);
    size_t bytes = (size_t)platform->width * rows * 2;
    platform->draw_buffer = malloc(bytes);
    if(!platform->draw_buffer) goto fail;
    lv_display_set_user_data(platform->display, platform);
    lv_display_set_flush_cb(platform->display, flush_cb);
    lv_display_set_buffers(platform->display, platform->draw_buffer, NULL, bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    platform->input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(platform->input_fd < 0) goto fail;
    return true;
fail:
    ebook_platform_destroy(platform);
    return false;
}

ebook_key_t ebook_platform_read_key(ebook_platform_t *platform)
{
    struct input_event event;
    while(read(platform->input_fd, &event, sizeof(event)) == sizeof(event)) {
        if(event.type != EV_KEY || event.value == 0) continue;
        switch(event.code) {
        case KEY_1: case KEY_UP: case KEY_LEFT: return EBOOK_KEY_PREV;
        case KEY_2: case KEY_DOWN: case KEY_RIGHT: return EBOOK_KEY_NEXT;
        case KEY_3: case KEY_ENTER: return EBOOK_KEY_ENTER;
        case KEY_4: case KEY_ESC: case KEY_BACK: return EBOOK_KEY_BACK;
        default: break;
        }
    }
    return EBOOK_KEY_NONE;
}

void ebook_platform_set_brightness(int level)
{
    FILE *fp = fopen("/sys/class/backlight/backlight/brightness", "w");
    if(fp) { fprintf(fp, "%d\n", level); fclose(fp); }
}

void ebook_platform_destroy(ebook_platform_t *platform)
{
    if(platform->input_fd >= 0) close(platform->input_fd);
    if(platform->display) lv_display_delete(platform->display);
    free(platform->draw_buffer);
    drm_warpper_destroy(&platform->drm);
    memset(platform, 0, sizeof(*platform));
    platform->input_fd = -1;
}
