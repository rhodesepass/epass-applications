#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include "hal_run.h"

#include <fcntl.h>
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
        hal_display_mount_layer(&platform->drm, HAL_DISPLAY_LAYER_UI, 0, 0, &platform->buffers[idx]);
    }
    lv_display_flush_ready(display);
}

bool tutorial_platform_init(tutorial_platform_t *platform)
{
    memset(platform, 0, sizeof(*platform));
    if(hal_display_init(&platform->drm) < 0) return false;
    hal_display_display_size(&platform->drm, &platform->width, &platform->height);
    if(hal_display_init_layer(&platform->drm, HAL_DISPLAY_LAYER_UI, platform->width, platform->height,
                              HAL_DISPLAY_LAYER_MODE_RGB565) < 0 ||
       hal_display_allocate_buffer(&platform->drm, HAL_DISPLAY_LAYER_UI, &platform->buffers[0]) < 0 ||
       hal_display_allocate_buffer(&platform->drm, HAL_DISPLAY_LAYER_UI, &platform->buffers[1]) < 0 ||
       hal_display_mount_layer(&platform->drm, HAL_DISPLAY_LAYER_UI, 0, 0, &platform->buffers[0]) < 0) {
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
    if(hal_input_init(&platform->input) <= 0) goto fail;
    /* 遮罩层拿不到就算了, 只是没有过渡动画, 不该让整个应用起不来。
     * allocate 取的是 init_layer 登记的宽高, 不 init 会拿 0×0 造 buffer */
    if(hal_display_layer_can_fade(&platform->drm, HAL_DISPLAY_LAYER_TOP) &&
       hal_display_init_layer(&platform->drm, HAL_DISPLAY_LAYER_TOP,
                              platform->width, platform->height,
                              HAL_DISPLAY_LAYER_MODE_RGB565) == 0 &&
       hal_display_allocate_buffer(&platform->drm, HAL_DISPLAY_LAYER_TOP,
                                   &platform->overlay_buf) == 0)
        platform->has_overlay = true;
    return true;
fail:
    tutorial_platform_destroy(platform);
    return false;
}

tutorial_key_t tutorial_platform_read_key(tutorial_platform_t *platform)
{
    hal_input_event_t ev;
    while(hal_input_next_event(&platform->input, &ev)) {
        if(!ev.pressed) continue;
        switch(ev.key) {
        case HAL_KEY_1: return TUTORIAL_KEY_PREV;
        case HAL_KEY_2: return TUTORIAL_KEY_NEXT;
        case HAL_KEY_3: return TUTORIAL_KEY_ENTER;
        case HAL_KEY_4: return TUTORIAL_KEY_BACK;
        default: break;
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
    if(hal_display_mount_layer_alpha(&platform->drm, HAL_DISPLAY_LAYER_TOP, 0, 0,
                                     &platform->overlay_buf, (uint8_t)from) < 0)
        return;
    const int step_ms = 16; /* 约 60Hz, 再密屏幕也跟不上 */
    int steps = ms / step_ms;
    if(steps < 1) steps = 1;
    for(int i = 1; i <= steps; i++) {
        /* hal_idle 而非裸 nanosleep: wasm 上前者才会让出主线程,
         * 浏览器才有机会把每一步 alpha 画出来 */
        hal_idle(step_ms);
        hal_display_set_layer_alpha(&platform->drm, HAL_DISPLAY_LAYER_TOP,
                                    (uint8_t)(from + (to - from) * i / steps));
    }
    /* 全透明就别继续占着 DE 的合成带宽 */
    if(to == 0) hal_display_disable_layer_sync(&platform->drm, HAL_DISPLAY_LAYER_TOP);
}

void tutorial_platform_set_brightness(int level)
{
    FILE *fp = fopen("/sys/class/backlight/backlight/brightness", "w");
    if(fp) { fprintf(fp, "%d\n", level); fclose(fp); }
}

void tutorial_platform_destroy(tutorial_platform_t *platform)
{
    hal_input_destroy(&platform->input);
    if(platform->display) lv_display_delete(platform->display);
    if(platform->has_overlay)
        hal_display_free_buffer(&platform->drm, HAL_DISPLAY_LAYER_TOP, &platform->overlay_buf);
    hal_display_free_buffer(&platform->drm, HAL_DISPLAY_LAYER_UI, &platform->buffers[0]);
    hal_display_free_buffer(&platform->drm, HAL_DISPLAY_LAYER_UI, &platform->buffers[1]);
    hal_display_destroy(&platform->drm);
    memset(platform, 0, sizeof(*platform));
}
