#include "epass_game.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef assert
#undef assert
#endif
#define assert(condition)                                                     \
    do {                                                                      \
        if(!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__,       \
                    __LINE__, #condition);                                    \
            abort();                                                          \
        }                                                                     \
    } while(0)

enum {
    GUARD_SIZE = 64,
    PITCH_PADDING = 28,
    SENTINEL_BYTE = 0xa5
};

typedef struct {
    uint8_t *allocation;
    size_t allocation_size;
    game_framebuffer_t fb;
} guarded_framebuffer_t;

static guarded_framebuffer_t framebuffer_create(int width, int height)
{
    guarded_framebuffer_t guarded = {0};
    const int pitch = width * (int)sizeof(uint32_t) + PITCH_PADDING;
    const size_t payload_size = (size_t)pitch * (size_t)height;

    guarded.allocation_size = GUARD_SIZE + payload_size + GUARD_SIZE;
    guarded.allocation = malloc(guarded.allocation_size);
    assert(guarded.allocation);
    memset(guarded.allocation, SENTINEL_BYTE, guarded.allocation_size);
    guarded.fb = (game_framebuffer_t){
        .pixels = (uint32_t *)(void *)(guarded.allocation + GUARD_SIZE),
        .width = width,
        .height = height,
        .pitch = pitch,
    };
    return guarded;
}

static void framebuffer_reset(guarded_framebuffer_t *guarded)
{
    memset(guarded->allocation, SENTINEL_BYTE, guarded->allocation_size);
}

static void assert_bytes_are_sentinel(const uint8_t *bytes, size_t size)
{
    for(size_t i = 0; i < size; i++) assert(bytes[i] == SENTINEL_BYTE);
}

static void assert_guards_and_padding(const guarded_framebuffer_t *guarded)
{
    const size_t payload_size =
        (size_t)guarded->fb.pitch * (size_t)guarded->fb.height;
    const uint8_t *payload = (const uint8_t *)(const void *)guarded->fb.pixels;

    assert_bytes_are_sentinel(guarded->allocation, GUARD_SIZE);
    assert_bytes_are_sentinel(payload + payload_size, GUARD_SIZE);
    for(int y = 0; y < guarded->fb.height; y++) {
        const uint8_t *padding =
            payload + (size_t)y * (size_t)guarded->fb.pitch +
            (size_t)guarded->fb.width * sizeof(uint32_t);
        assert_bytes_are_sentinel(padding, PITCH_PADDING);
    }
}

static void assert_active_is_color(const guarded_framebuffer_t *guarded,
                                   uint32_t color)
{
    for(int y = 0; y < guarded->fb.height; y++) {
        const uint32_t *row = (const uint32_t *)(const void *)(
            (const uint8_t *)(const void *)guarded->fb.pixels +
            (size_t)y * (size_t)guarded->fb.pitch);
        for(int x = 0; x < guarded->fb.width; x++) assert(row[x] == color);
    }
}

static size_t count_active_color(const guarded_framebuffer_t *guarded,
                                 uint32_t color)
{
    size_t count = 0;
    for(int y = 0; y < guarded->fb.height; y++) {
        const uint32_t *row = (const uint32_t *)(const void *)(
            (const uint8_t *)(const void *)guarded->fb.pixels +
            (size_t)y * (size_t)guarded->fb.pitch);
        for(int x = 0; x < guarded->fb.width; x++)
            if(row[x] == color) count++;
    }
    return count;
}

static void test_resolution(int width, int height)
{
    const uint32_t fill_color = 0xff123456u;
    const uint32_t rect_color = 0xffabcdefu;
    const uint32_t text_color = 0xffffffffu;
    guarded_framebuffer_t guarded = framebuffer_create(width, height);

    assert(game_logical_x(&guarded.fb, 0) == 0);
    assert(game_logical_x(&guarded.fb, GAME_LOGICAL_WIDTH) == width);
    assert(game_logical_x(&guarded.fb, GAME_LOGICAL_WIDTH / 2) == width / 2);
    assert(game_logical_y(&guarded.fb, 0) == 0);
    assert(game_logical_y(&guarded.fb, GAME_LOGICAL_HEIGHT) == height);
    assert(game_logical_y(&guarded.fb, GAME_LOGICAL_HEIGHT / 2) ==
           height / 2);
    assert(game_logical_x(NULL, GAME_LOGICAL_WIDTH) == 0);
    assert(game_logical_y(NULL, GAME_LOGICAL_HEIGHT) == 0);

    game_draw_fill(&guarded.fb, fill_color);
    assert_active_is_color(&guarded, fill_color);
    assert_guards_and_padding(&guarded);

    framebuffer_reset(&guarded);
    game_draw_rect(&guarded.fb, 0, 0, GAME_LOGICAL_WIDTH,
                   GAME_LOGICAL_HEIGHT, rect_color);
    assert_active_is_color(&guarded, rect_color);
    assert_guards_and_padding(&guarded);

    framebuffer_reset(&guarded);
    game_draw_rect(&guarded.fb, -20, -20, 40, 40, rect_color);
    game_draw_rect(&guarded.fb, GAME_LOGICAL_WIDTH - 20,
                   GAME_LOGICAL_HEIGHT - 20, 40, 40, rect_color);
    assert(count_active_color(&guarded, rect_color) > 0);
    assert_guards_and_padding(&guarded);

    framebuffer_reset(&guarded);
    game_draw_text(&guarded.fb, -4, -4, "EDGE", 3, text_color);
    game_draw_text(&guarded.fb, GAME_LOGICAL_WIDTH - 3,
                   GAME_LOGICAL_HEIGHT - 3, "X", 4, text_color);
    assert(count_active_color(&guarded, text_color) > 0);
    assert_guards_and_padding(&guarded);

    free(guarded.allocation);
}

int main(void)
{
    test_resolution(360, 640);
    test_resolution(480, 854);
    test_resolution(720, 1280);
    puts("epass_game draw tests passed");
    return 0;
}
