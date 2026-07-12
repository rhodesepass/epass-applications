#include "niccc.h"

#include <assert.h>
#include <limits.h>
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

#ifndef NICCC_TEST_SCENE_PATH
#define NICCC_TEST_SCENE_PATH "scene1.bin"
#endif

enum {
    GUARD_SIZE = 64,
    PITCH_PADDING = 36,
    SENTINEL_BYTE = 0x5a
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

static void assert_drawing_changed(const guarded_framebuffer_t *guarded)
{
    const uint32_t sentinel = 0x5a5a5a5au;
    size_t changed = 0;

    for(int y = 0; y < guarded->fb.height; y++) {
        const uint32_t *row = (const uint32_t *)(const void *)(
            (const uint8_t *)(const void *)guarded->fb.pixels +
            (size_t)y * (size_t)guarded->fb.pitch);
        for(int x = 0; x < guarded->fb.width; x++)
            if(row[x] != sentinel) changed++;
    }
    assert(changed > 0);
}

static void draw_and_check(guarded_framebuffer_t *guarded, int frame)
{
    framebuffer_reset(guarded);
    niccc_draw_frame(&guarded->fb, frame);
    assert_drawing_changed(guarded);
    assert_guards_and_padding(guarded);
}

static void test_resolution(int width, int height)
{
    guarded_framebuffer_t guarded = framebuffer_create(width, height);

    draw_and_check(&guarded, 0);
    draw_and_check(&guarded, INT_MAX);
    draw_and_check(&guarded, 0);

    free(guarded.allocation);
}

int main(void)
{
    assert(niccc_init(NICCC_TEST_SCENE_PATH));
    test_resolution(360, 640);
    test_resolution(480, 854);
    test_resolution(720, 1280);
    niccc_destroy();
    puts("epniccc renderer tests passed");
    return 0;
}
