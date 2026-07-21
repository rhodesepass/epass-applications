#include "epass_game.h"
#include "epniccc_bg.h"
#include "niccc.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

static uint32_t rgb565_to_argb(const unsigned char *pixel)
{
    uint16_t value = (uint16_t)pixel[0] |
                     ((uint16_t)pixel[1] << 8);
    uint8_t r = (uint8_t)(((value >> 11) & 0x1f) << 3);
    uint8_t g = (uint8_t)(((value >> 5) & 0x3f) << 2);
    uint8_t b = (uint8_t)((value & 0x1f) << 3);
    return 0xff000000u | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | b;
}

static void draw_background(game_framebuffer_t *fb)
{
    for(int y = 0; y < fb->height; y++) {
        int source_y = (int)((int64_t)y * GAME_LOGICAL_HEIGHT /
                             fb->height);
        uint32_t *row = (uint32_t *)((uint8_t *)fb->pixels +
                                     (size_t)y * fb->pitch);
        for(int x = 0; x < fb->width; x++) {
            int source_x = (int)((int64_t)x * GAME_LOGICAL_WIDTH /
                                 fb->width);
            size_t offset =
                ((size_t)source_y * GAME_LOGICAL_WIDTH + source_x) * 2;
            row[x] = rgb565_to_argb(gImage_epniccc_bg + offset);
        }
    }
}

static const char *pressed_key_name(const game_platform_t *platform)
{
    if(game_key_pressed(platform, GAME_KEY_OK)) return "KEY_3";
    if(game_key_pressed(platform, GAME_KEY_DOWN)) return "KEY_2";
    if(game_key_pressed(platform, GAME_KEY_UP)) return "KEY_1";
    return " ";
}

typedef struct {
    game_platform_t platform;
    void *initialized_buffers[2];
    uint64_t start_ms;
    uint64_t elapsed_ms;
    bool timing;
    int frame_count;
} epniccc_app_t;

static bool tick(void *userdata)
{
    epniccc_app_t *app = userdata;

    if(!running) return false;
    game_input_update(&app->platform);
    if(game_key_pressed(&app->platform, GAME_KEY_BACK)) return false;
    const char *key_name = pressed_key_name(&app->platform);

    game_framebuffer_t fb;
    if(!game_platform_acquire_frame(&app->platform, &fb)) return false;
    if(fb.pixels != app->initialized_buffers[0] &&
       fb.pixels != app->initialized_buffers[1]) {
        draw_background(&fb);
        if(!app->initialized_buffers[0])
            app->initialized_buffers[0] = fb.pixels;
        else
            app->initialized_buffers[1] = fb.pixels;
    }

    game_draw_rect(&fb, 0, 108, GAME_LOGICAL_WIDTH, 62, 0xff000000);
    game_draw_text(&fb, 10, 112, "FRAME:", 2, 0xffffffff);
    game_draw_number(&fb, 82, 112, app->frame_count, 2, 0xffffffff);
    game_draw_text(&fb, 154, 112, "TIME:", 2, 0xffffffff);
    if(app->timing || app->frame_count % 100 >= 50)
        game_draw_number(&fb, 214, 112,
                         app->timing
                             ? (int)(game_monotonic_ms() - app->start_ms)
                             : (int)app->elapsed_ms,
                         2, 0xffffffff);
    game_draw_text(&fb, 10, 136, "PRESSED KEY:", 2, 0xffffffff);
    game_draw_text(&fb, 154, 136, key_name, 2, 0xffffffff);

    niccc_draw_frame(&fb, app->frame_count);
    if(!game_platform_present(&app->platform)) return false;

    app->frame_count++;
    if(app->frame_count >= 1800) {
        app->frame_count = 0;
        if(app->timing) {
            app->elapsed_ms = game_monotonic_ms() - app->start_ms;
            app->timing = false;
        }
    }
    return true;
}

int main(void)
{
    static epniccc_app_t app = {.timing = true};
    struct sigaction action = {0};
    int result = 1;

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if(!niccc_init("scene1.bin")) return 1;
    /* niccc writes 32-bit pixels straight into the framebuffer. */
    if(!game_platform_init_ex(&app.platform, GAME_PIXEL_FORMAT_ARGB8888))
        goto cleanup_scene;
    app.start_ms = game_monotonic_ms();

    game_run(&app.platform, tick, &app);

    result = 0;
    game_platform_destroy(&app.platform);
cleanup_scene:
    niccc_destroy();
    return result;
}
