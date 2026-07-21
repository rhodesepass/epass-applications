#include "../include/epass_game.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Public-domain 5x7 font, columns stored least-significant pixel first. */
static const uint8_t font5x7[96][5] = {
 {0,0,0,0,0},{0,0,95,0,0},{0,7,0,7,0},{20,127,20,127,20},
 {36,42,127,42,18},{35,19,8,100,98},{54,73,85,34,80},{0,5,3,0,0},
 {0,28,34,65,0},{0,65,34,28,0},{20,8,62,8,20},{8,8,62,8,8},
 {0,80,48,0,0},{8,8,8,8,8},{0,96,96,0,0},{32,16,8,4,2},
 {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
 {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,41,30},{0,54,54,0,0},{0,86,54,0,0},
 {8,20,34,65,0},{20,20,20,20,20},{0,65,34,20,8},{2,1,81,9,6},
 {50,73,121,65,62},{126,17,17,17,126},{127,73,73,73,54},
 {62,65,65,65,34},{127,65,65,34,28},{127,73,73,73,65},
 {127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},
 {127,64,64,64,64},{127,2,12,2,127},{127,4,8,16,127},
 {62,65,65,65,62},{127,9,9,9,6},{62,65,81,33,94},
 {127,9,25,41,70},{70,73,73,73,49},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},
 {99,20,8,20,99},{3,4,120,4,3},{97,81,73,69,67},
 {0,127,65,65,0},{2,4,8,16,32},{0,65,65,127,0},{4,2,1,2,4},
 {64,64,64,64,64},{0,1,2,4,0},{32,84,84,84,120},
 {127,72,68,68,56},{56,68,68,68,32},{56,68,68,72,127},
 {56,84,84,84,24},{8,126,9,1,2},{12,82,82,82,62},
 {127,8,4,4,120},{0,68,125,64,0},{32,64,68,61,0},
 {127,16,40,68,0},{0,65,127,64,0},{124,4,24,4,120},
 {124,8,4,4,120},{56,68,68,68,56},{124,20,20,20,8},
 {8,20,20,24,124},{124,8,4,4,8},{72,84,84,84,32},
 {4,63,68,64,32},{60,64,64,32,124},{28,32,64,32,28},
 {60,64,48,64,60},{68,40,16,40,68},{12,80,80,80,60},
 {68,100,84,76,68},{0,8,54,65,0},{0,0,127,0,0},
 {0,65,54,8,0},{2,1,2,4,2},{127,127,127,127,127}
};

static inline uint16_t argb_to_rgb565(uint32_t argb)
{
    return (uint16_t)(((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) |
                      ((argb >> 3) & 0x001f));
}

static inline int fb_bytes_per_pixel(const game_framebuffer_t *fb)
{
    return fb->format == GAME_PIXEL_FORMAT_RGB565 ? 2 : 4;
}

uint64_t game_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void game_draw_fill(game_framebuffer_t *fb, uint32_t argb)
{
    if(!fb || !fb->pixels || fb->pitch < fb->width * fb_bytes_per_pixel(fb))
        return;
    if(fb->format == GAME_PIXEL_FORMAT_RGB565) {
        uint16_t c = argb_to_rgb565(argb);
        for(int y = 0; y < fb->height; y++) {
            uint16_t *row = (uint16_t *)((uint8_t *)fb->pixels +
                                         (size_t)y * fb->pitch);
            for(int x = 0; x < fb->width; x++) row[x] = c;
        }
        return;
    }
    for(int y = 0; y < fb->height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fb->pixels +
                                     (size_t)y * fb->pitch);
        for(int x = 0; x < fb->width; x++) row[x] = argb;
    }
}

void game_draw_rect_px(game_framebuffer_t *fb, int x, int y, int width,
                       int height, uint32_t argb)
{
    if(!fb || !fb->pixels || width <= 0 || height <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + width > fb->width ? fb->width : x + width;
    int y1 = y + height > fb->height ? fb->height : y + height;
    if(fb->format == GAME_PIXEL_FORMAT_RGB565) {
        uint16_t c = argb_to_rgb565(argb);
        for(int py = y0; py < y1; py++) {
            uint16_t *row = (uint16_t *)((uint8_t *)fb->pixels +
                                         (size_t)py * fb->pitch);
            for(int px = x0; px < x1; px++) row[px] = c;
        }
        return;
    }
    for(int py = y0; py < y1; py++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fb->pixels +
                                     (size_t)py * fb->pitch);
        for(int px = x0; px < x1; px++) row[px] = argb;
    }
}

static void put_pixel(game_framebuffer_t *fb, int x, int y, uint32_t argb)
{
    if(x < 0 || y < 0 || x >= fb->width || y >= fb->height) return;
    uint8_t *base = (uint8_t *)fb->pixels + (size_t)y * fb->pitch;
    if(fb->format == GAME_PIXEL_FORMAT_RGB565)
        ((uint16_t *)base)[x] = argb_to_rgb565(argb);
    else
        ((uint32_t *)base)[x] = argb;
}

void game_draw_line_px(game_framebuffer_t *fb, int x0, int y0, int x1,
                       int y1, uint32_t argb)
{
    if(!fb || !fb->pixels) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for(;;) {
        put_pixel(fb, x0, y0, argb);
        if(x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if(twice >= dy) { error += dy; x0 += sx; }
        if(twice <= dx) { error += dx; y0 += sy; }
    }
}

int game_logical_x(const game_framebuffer_t *fb, int x)
{
    return fb ? (int)((int64_t)x * fb->width / GAME_LOGICAL_WIDTH) : 0;
}

int game_logical_y(const game_framebuffer_t *fb, int y)
{
    return fb ? (int)((int64_t)y * fb->height / GAME_LOGICAL_HEIGHT) : 0;
}

void game_draw_rect(game_framebuffer_t *fb, int x, int y, int width,
                    int height, uint32_t argb)
{
    int x0 = game_logical_x(fb, x), y0 = game_logical_y(fb, y);
    int x1 = game_logical_x(fb, x + width);
    int y1 = game_logical_y(fb, y + height);
    game_draw_rect_px(fb, x0, y0, x1 - x0, y1 - y0, argb);
}

static void draw_glyph(game_framebuffer_t *fb, int x, int y, unsigned char c,
                       int scale, uint32_t argb)
{
    if(c < 32 || c > 127) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for(int column = 0; column < 5; column++)
        for(int row = 0; row < 7; row++)
            if(glyph[column] & (1u << row))
                game_draw_rect(fb, x + column * scale, y + row * scale,
                               scale, scale, argb);
}

void game_draw_text(game_framebuffer_t *fb, int x, int y, const char *text,
                    int scale, uint32_t argb)
{
    if(!fb || !text || scale <= 0) return;
    int origin = x;
    for(; *text; text++) {
        if(*text == '\n') {
            x = origin;
            y += 8 * scale;
            continue;
        }
        draw_glyph(fb, x, y, (unsigned char)*text, scale, argb);
        x += 6 * scale;
    }
}

void game_draw_number(game_framebuffer_t *fb, int x, int y, int value,
                      int scale, uint32_t argb)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    game_draw_text(fb, x, y, text, scale, argb);
}
