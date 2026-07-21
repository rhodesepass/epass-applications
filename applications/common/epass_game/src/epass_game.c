#include "../include/epass_game.h"
#include "hal_display.h"
#include "hal_input.h"
#include "log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GAME_LAYER HAL_DISPLAY_LAYER_OVERLAY

typedef struct {
    hal_display_t drm;
    hal_buffer_t buffers[2];
    hal_display_queue_item_t items[2];
    hal_display_queue_item_t *acquired;
    hal_input_t input;
    int width, height;
    game_pixel_format_t format;
    bool drm_ready;
    bool layer_ready;
    bool down[GAME_KEY_COUNT];
    bool pressed[GAME_KEY_COUNT];
    bool repeated[GAME_KEY_COUNT];
    uint64_t next_repeat[GAME_KEY_COUNT];
    uint32_t repeat_delay, repeat_interval;
} game_platform_impl_t;

static game_platform_impl_t *get_impl(const game_platform_t *platform)
{
    return platform ? (game_platform_impl_t *)platform->impl : NULL;
}

bool game_platform_init(game_platform_t *platform)
{
    return game_platform_init_ex(platform, GAME_PIXEL_FORMAT_RGB565);
}

bool game_platform_init_ex(game_platform_t *platform,
                           game_pixel_format_t format)
{
    game_platform_impl_t *impl;
    if(!platform || platform->impl) return false;
    impl = calloc(1, sizeof(*impl));
    if(!impl) return false;
    platform->impl = impl;
    impl->repeat_delay = 350;
    impl->repeat_interval = 90;
    impl->format = format;

    if(hal_display_init(&impl->drm) < 0) goto fail;
    impl->drm_ready = true;
    hal_display_display_size(&impl->drm, &impl->width, &impl->height);
    hal_display_layer_mode_t layer_mode =
        format == GAME_PIXEL_FORMAT_RGB565 ? HAL_DISPLAY_LAYER_MODE_RGB565
                                           : HAL_DISPLAY_LAYER_MODE_ARGB8888;
    if(hal_display_init_layer(&impl->drm, GAME_LAYER, impl->width,
                              impl->height, layer_mode) < 0)
        goto fail;
    impl->layer_ready = true;
    for(int i = 0; i < 2; i++) {
        if(hal_display_allocate_buffer(&impl->drm, GAME_LAYER,
                                       &impl->buffers[i]) < 0)
            goto fail;
        game_framebuffer_t fb = {
            .pixels = (uint32_t *)impl->buffers[i].vaddr,
            .width = impl->width,
            .height = impl->height,
            .pitch = (int)impl->buffers[i].pitch,
            .format = format
        };
        game_draw_fill(&fb, 0xff000000);
        impl->items[i].type = HAL_DISPLAY_ITEM_FLIP_FB;
        impl->items[i].fb_id = impl->buffers[i].fb_id;
        impl->items[i].userdata = &impl->buffers[i];
        impl->items[i].on_heap = false;
    }
    if(hal_display_mount_layer(&impl->drm, GAME_LAYER, 0, 0,
                               &impl->buffers[0]) < 0)
        goto fail;
    if(hal_display_enqueue_display_item(&impl->drm, GAME_LAYER,
                                        &impl->items[0]) ||
       hal_display_enqueue_display_item(&impl->drm, GAME_LAYER,
                                        &impl->items[1]))
        goto fail;
    if(hal_input_init(&impl->input) <= 0) {
        log_error("no /dev/input/event* with nav keys found");
        goto fail;
    }
    return true;
fail:
    game_platform_destroy(platform);
    return false;
}

bool game_platform_acquire_frame(game_platform_t *platform,
                                 game_framebuffer_t *framebuffer)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl || !framebuffer || impl->acquired) return false;
    if(hal_display_dequeue_free_item(&impl->drm, GAME_LAYER,
                                     &impl->acquired) != 0)
        return false;
    hal_buffer_t *buffer = impl->acquired->userdata;
    framebuffer->pixels = (uint32_t *)buffer->vaddr;
    framebuffer->width = (int)buffer->width;
    framebuffer->height = (int)buffer->height;
    framebuffer->pitch = (int)buffer->pitch;
    framebuffer->format = impl->format;
    return true;
}

bool game_platform_present(game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl || !impl->acquired) return false;
    hal_display_queue_item_t *item = impl->acquired;
    item->type = HAL_DISPLAY_ITEM_FLIP_FB;
    item->fb_id = ((hal_buffer_t *)item->userdata)->fb_id;
    if(hal_display_enqueue_display_item(&impl->drm, GAME_LAYER, item))
        return false;
    impl->acquired = NULL;
    return true;
}

void game_platform_destroy(game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl) return;
    hal_input_destroy(&impl->input);
    if(impl->drm_ready) {
        hal_display_stop(&impl->drm);
        if(impl->layer_ready)
            hal_display_disable_layer_sync(&impl->drm, GAME_LAYER);
        for(int i = 0; i < 2; i++)
            hal_display_free_buffer(&impl->drm, GAME_LAYER,
                                    &impl->buffers[i]);
        hal_display_destroy(&impl->drm);
    }
    free(impl);
    platform->impl = NULL;
}

int game_platform_width(const game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl ? impl->width : 0;
}

int game_platform_height(const game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl ? impl->height : 0;
}

void game_input_update(game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    hal_input_event_t ev;
    if(!impl) return;
    memset(impl->pressed, 0, sizeof(impl->pressed));
    memset(impl->repeated, 0, sizeof(impl->repeated));
    uint64_t now = game_monotonic_ms();
    while(hal_input_next_event(&impl->input, &ev)) {
        /* HAL_KEY_1..4 与 GAME_KEY_UP/DOWN/OK/BACK 同序。
         * 内核 autorepeat 不参与: 重复节奏由下面的帧级状态机自己掌控 */
        int key = (int)ev.key;
        if(ev.repeat) continue;
        if(ev.pressed) {
            if(!impl->down[key]) impl->pressed[key] = true;
            impl->down[key] = true;
            impl->next_repeat[key] = now + impl->repeat_delay;
        } else {
            impl->down[key] = false;
        }
    }
    for(int key = 0; key < GAME_KEY_COUNT; key++) {
        if(impl->down[key] && now >= impl->next_repeat[key]) {
            impl->repeated[key] = true;
            do {
                impl->next_repeat[key] += impl->repeat_interval;
            } while(now >= impl->next_repeat[key]);
        }
    }
}

void game_input_set_repeat(game_platform_t *platform, uint32_t delay_ms,
                           uint32_t interval_ms)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl) return;
    impl->repeat_delay = delay_ms;
    impl->repeat_interval = interval_ms ? interval_ms : 1;
}

static bool key_flag(const bool *flags, game_key_t key)
{
    return key >= 0 && key < GAME_KEY_COUNT && flags[key];
}

bool game_key_down(const game_platform_t *platform, game_key_t key)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl && key_flag(impl->down, key);
}

bool game_key_pressed(const game_platform_t *platform, game_key_t key)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl && key_flag(impl->pressed, key);
}

bool game_key_repeated(const game_platform_t *platform, game_key_t key)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl && key_flag(impl->repeated, key);
}


void game_run(game_platform_t *platform, game_tick_fn tick, void *userdata)
{
    (void)platform;
    while(tick(userdata)) {}
}

void game_platform_idle(game_platform_t *platform, uint32_t ms)
{
    (void)platform;
    usleep(ms * 1000);
}
