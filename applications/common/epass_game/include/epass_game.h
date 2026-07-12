#ifndef EPASS_GAME_H
#define EPASS_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_LOGICAL_WIDTH 360
#define GAME_LOGICAL_HEIGHT 640

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int pitch;
} game_framebuffer_t;

typedef enum {
    GAME_KEY_UP = 0,
    GAME_KEY_DOWN,
    GAME_KEY_OK,
    GAME_KEY_BACK,
    GAME_KEY_COUNT
} game_key_t;

typedef struct {
    void *impl;
} game_platform_t;

/* Opens DRM/input, selects the connected display mode and starts layer 1. */
bool game_platform_init(game_platform_t *platform);
/* Blocks until a back buffer is safe for CPU drawing. */
bool game_platform_acquire_frame(game_platform_t *platform,
                                 game_framebuffer_t *framebuffer);
/* Presents the frame returned by the last successful acquire call. */
bool game_platform_present(game_platform_t *platform);
void game_platform_destroy(game_platform_t *platform);

int game_platform_width(const game_platform_t *platform);
int game_platform_height(const game_platform_t *platform);
uint64_t game_monotonic_ms(void);

/* Drain nonblocking evdev input and update edge/repeat state once per tick. */
void game_input_update(game_platform_t *platform);
void game_input_set_repeat(game_platform_t *platform,
                           uint32_t delay_ms, uint32_t interval_ms);
bool game_key_down(const game_platform_t *platform, game_key_t key);
bool game_key_pressed(const game_platform_t *platform, game_key_t key);
bool game_key_repeated(const game_platform_t *platform, game_key_t key);

/* Physical-pixel drawing; all functions clip to the framebuffer. */
void game_draw_fill(game_framebuffer_t *fb, uint32_t argb);
void game_draw_rect_px(game_framebuffer_t *fb, int x, int y, int width,
                       int height, uint32_t argb);
void game_draw_line_px(game_framebuffer_t *fb, int x0, int y0, int x1,
                       int y1, uint32_t argb);

/* Drawing in the fixed 360x640 logical coordinate system. */
void game_draw_rect(game_framebuffer_t *fb, int x, int y, int width,
                    int height, uint32_t argb);
void game_draw_text(game_framebuffer_t *fb, int x, int y, const char *text,
                    int scale, uint32_t argb);
void game_draw_number(game_framebuffer_t *fb, int x, int y, int value,
                      int scale, uint32_t argb);
int game_logical_x(const game_framebuffer_t *fb, int x);
int game_logical_y(const game_framebuffer_t *fb, int y);

#ifdef __cplusplus
}
#endif
#endif
