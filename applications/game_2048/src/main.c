#include "epass_game.h"
#include "logic.h"

#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define HOLD_TO_RESTART_MS 1000u

static volatile sig_atomic_t running = 1;

static const char *const direction_names[GAME_2048_DIRECTION_COUNT] = {
    "UP", "RIGHT", "DOWN", "LEFT"
};

static const uint32_t tile_colors[] = {
    0xffcdc1b4, 0xffeee4da, 0xffede0c8, 0xfff2b179,
    0xfff59563, 0xfff67c5f, 0xfff65e3b, 0xffedcf72,
    0xffedcc61, 0xffedc850, 0xffedc53f, 0xffedc22e
};

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int text_width(const char *text, int scale)
{
    return text ? (int)strlen(text) * 6 * scale - scale : 0;
}

static void draw_centered(game_framebuffer_t *fb, int y, const char *text,
                          int scale, uint32_t color)
{
    game_draw_text(fb, (GAME_LOGICAL_WIDTH - text_width(text, scale)) / 2,
                   y, text, scale, color);
}

static unsigned tile_color_index(uint32_t value)
{
    unsigned index = 0;
    while(value > 1 && index + 1 < sizeof(tile_colors) / sizeof(tile_colors[0])) {
        value >>= 1;
        index++;
    }
    return index;
}

static void draw_tile(game_framebuffer_t *fb, int x, int y, uint32_t value)
{
    const int tile_size = 76;
    uint32_t background = value ? tile_colors[tile_color_index(value)]
                                : tile_colors[0];
    game_draw_rect(fb, x, y, tile_size, tile_size, background);
    if(value == 0) return;

    char text[16];
    snprintf(text, sizeof(text), "%" PRIu32, value);
    int digits = (int)strlen(text);
    int scale = digits <= 2 ? 5 : digits <= 4 ? 4 : digits <= 5 ? 3 : 2;
    int width = text_width(text, scale);
    int height = 7 * scale;
    uint32_t foreground = value <= 4 ? 0xff776e65 : 0xfff9f6f2;
    game_draw_text(fb, x + (tile_size - width) / 2,
                   y + (tile_size - height) / 2, text, scale, foreground);
}

static void draw_screen(game_framebuffer_t *fb, const game_2048_t *game,
                        game_2048_direction_t direction)
{
    const int board_x = 14;
    const int board_y = 132;
    const int gap = 6;
    const int tile_size = 76;
    const int board_size = gap + GAME_2048_SIZE * (tile_size + gap);
    char text[48];

    game_draw_fill(fb, 0xfffaf8ef);
    game_draw_text(fb, 14, 18, "2048", 7, 0xff776e65);

    snprintf(text, sizeof(text), "SCORE  %" PRIu32, game->score);
    game_draw_text(fb, 14, 82, text, 3, 0xff776e65);

    snprintf(text, sizeof(text), "DIRECTION: %s", direction_names[direction]);
    draw_centered(fb, 108, text, 2, 0xff8f7a66);

    game_draw_rect(fb, board_x, board_y, board_size, board_size, 0xffbbada0);
    for(int row = 0; row < GAME_2048_SIZE; row++)
        for(int column = 0; column < GAME_2048_SIZE; column++)
            draw_tile(fb, board_x + gap + column * (tile_size + gap),
                      board_y + gap + row * (tile_size + gap),
                      game->cells[row][column]);

    if(game->won) {
        game_draw_rect(fb, 18, 480, 324, 70, 0xffedc22e);
        draw_centered(fb, 491, "YOU WIN!", 5, 0xffffffff);
        draw_centered(fb, 532, "KEY 3: NEW GAME", 2, 0xffffffff);
    } else if(!game_2048_can_move(game)) {
        game_draw_rect(fb, 18, 480, 324, 70, 0xff776e65);
        draw_centered(fb, 491, "NO MOVES", 5, 0xffffffff);
        draw_centered(fb, 532, "KEY 3: NEW GAME", 2, 0xffffffff);
    } else {
        draw_centered(fb, 492, "KEY 1/2: SELECT", 2, 0xff776e65);
        draw_centered(fb, 516, "KEY 3: MOVE", 2, 0xff776e65);
        draw_centered(fb, 540, "HOLD KEY 3: NEW GAME", 2, 0xff776e65);
    }
    draw_centered(fb, 594, "KEY 4: EXIT", 2, 0xff776e65);
}

static bool render(game_platform_t *platform, const game_2048_t *game,
                   game_2048_direction_t direction)
{
    game_framebuffer_t framebuffer;
    if(!game_platform_acquire_frame(platform, &framebuffer)) return false;
    draw_screen(&framebuffer, game, direction);
    return game_platform_present(platform);
}

int main(void)
{
    game_platform_t platform = {0};
    game_2048_t game;
    game_2048_direction_t direction = GAME_2048_UP;
    bool dirty = true;
    bool ok_was_down = false;
    bool ok_consumed = false;
    uint64_t ok_started = 0;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    if(!game_platform_init(&platform)) {
        fprintf(stderr, "game_2048: platform initialization failed\n");
        return 1;
    }

    game_2048_reset(&game, (uint32_t)(game_monotonic_ms() ^ (uint64_t)getpid()));
    while(running) {
        game_input_update(&platform);

        if(game_key_pressed(&platform, GAME_KEY_BACK)) running = 0;
        if(game_key_pressed(&platform, GAME_KEY_UP)) {
            direction = (direction + GAME_2048_DIRECTION_COUNT - 1) %
                        GAME_2048_DIRECTION_COUNT;
            dirty = true;
        }
        if(game_key_pressed(&platform, GAME_KEY_DOWN)) {
            direction = (direction + 1) % GAME_2048_DIRECTION_COUNT;
            dirty = true;
        }

        bool ok_down = game_key_down(&platform, GAME_KEY_OK);
        bool terminal = game.won || !game_2048_can_move(&game);
        if(game_key_pressed(&platform, GAME_KEY_OK)) {
            ok_started = game_monotonic_ms();
            ok_consumed = false;
            if(terminal) {
                game_2048_reset(&game, (uint32_t)ok_started);
                ok_consumed = true;
                dirty = true;
            }
        }
        if(ok_down && !ok_consumed &&
           game_monotonic_ms() - ok_started >= HOLD_TO_RESTART_MS) {
            game_2048_reset(&game, (uint32_t)game_monotonic_ms());
            ok_consumed = true;
            dirty = true;
        }
        if(ok_was_down && !ok_down && !ok_consumed) {
            if(game_2048_move(&game, direction)) dirty = true;
            ok_consumed = true;
        }
        ok_was_down = ok_down;

        if(dirty && running) {
            if(!render(&platform, &game, direction)) {
                fprintf(stderr, "game_2048: frame presentation failed\n");
                running = 0;
            }
            dirty = false;
        }
        usleep(10000);
    }

    game_platform_destroy(&platform);
    return 0;
}
