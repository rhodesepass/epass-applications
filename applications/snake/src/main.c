#include "epass_game.h"
#include "logic.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BOARD_X 20
#define BOARD_Y 100
#define CELL_SIZE 20
#define BOARD_WIDTH (SNAKE_GRID_WIDTH * CELL_SIZE)
#define BOARD_HEIGHT (SNAKE_GRID_HEIGHT * CELL_SIZE)

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void draw_centered(game_framebuffer_t *fb, int y, const char *text,
                          int scale, uint32_t color)
{
    int width = (int)strlen(text) * 6 * scale;
    game_draw_text(fb, (GAME_LOGICAL_WIDTH - width) / 2, y, text, scale,
                   color);
}

static void draw_scene(game_framebuffer_t *fb, const snake_game_t *game)
{
    const uint32_t background = 0xff07131au;
    const uint32_t panel = 0xff102630u;
    const uint32_t border = 0xff3a6874u;
    const uint32_t snake = 0xff35c96fu;
    const uint32_t head = 0xffa4f06du;
    const uint32_t food = 0xffff5c5cu;
    const uint32_t text = 0xffe8f2f2u;
    const uint32_t muted = 0xff91aeb5u;

    game_draw_fill(fb, background);
    draw_centered(fb, 22, "SNAKE", 4, head);
    game_draw_text(fb, 20, 68, "SCORE", 2, muted);
    game_draw_number(fb, 98, 68, game->score, 2, text);

    game_draw_rect(fb, BOARD_X - 3, BOARD_Y - 3,
                   BOARD_WIDTH + 6, BOARD_HEIGHT + 6, border);
    game_draw_rect(fb, BOARD_X, BOARD_Y, BOARD_WIDTH, BOARD_HEIGHT, panel);

    game_draw_rect(fb,
                   BOARD_X + game->food.x * CELL_SIZE + 4,
                   BOARD_Y + game->food.y * CELL_SIZE + 4,
                   CELL_SIZE - 8, CELL_SIZE - 8, food);

    for(size_t i = game->length; i-- > 0;) {
        const snake_point_t part = game->body[i];
        game_draw_rect(fb, BOARD_X + part.x * CELL_SIZE + 2,
                       BOARD_Y + part.y * CELL_SIZE + 2,
                       CELL_SIZE - 4, CELL_SIZE - 4,
                       i == 0 ? head : snake);
    }

    if(game->state != SNAKE_STATE_RUNNING) {
        game_draw_rect(fb, 58, 276, 244, 88, 0xee07131au);
        if(game->state == SNAKE_STATE_READY) {
            draw_centered(fb, 292, "READY", 3, text);
            draw_centered(fb, 330, "PRESS 3 TO START", 1, muted);
        } else if(game->state == SNAKE_STATE_PAUSED) {
            draw_centered(fb, 292, "PAUSED", 3, text);
            draw_centered(fb, 330, "PRESS 3 TO RESUME", 1, muted);
        } else {
            draw_centered(fb, 286, game->won ? "YOU WIN" : "GAME OVER",
                          3, game->won ? head : food);
            draw_centered(fb, 330, "PRESS 3 TO RESTART", 1, muted);
        }
    }

    draw_centered(fb, 535, "1 TURN LEFT   2 TURN RIGHT", 1, text);
    draw_centered(fb, 563, "3 START / PAUSE", 1, text);
    draw_centered(fb, 591, "4 EXIT", 1, muted);
}

typedef struct {
    game_platform_t platform;
    snake_game_t game;
    uint64_t next_step;
} snake_app_t;

static bool tick(void *userdata)
{
    snake_app_t *app = userdata;
    game_framebuffer_t framebuffer;

    if(!running) return false;
    game_input_update(&app->platform);
    if(game_key_pressed(&app->platform, GAME_KEY_BACK)) return false;

    if(game_key_pressed(&app->platform, GAME_KEY_UP) ||
       game_key_repeated(&app->platform, GAME_KEY_UP))
        snake_game_turn_left(&app->game);
    if(game_key_pressed(&app->platform, GAME_KEY_DOWN) ||
       game_key_repeated(&app->platform, GAME_KEY_DOWN))
        snake_game_turn_right(&app->game);

    if(game_key_pressed(&app->platform, GAME_KEY_OK)) {
        if(app->game.state == SNAKE_STATE_READY) {
            snake_game_start(&app->game);
        } else if(app->game.state == SNAKE_STATE_GAME_OVER) {
            snake_game_init(&app->game, (uint32_t)game_monotonic_ms());
            snake_game_start(&app->game);
        } else {
            snake_game_toggle_pause(&app->game);
        }
        app->next_step = game_monotonic_ms();
    }

    uint64_t now = game_monotonic_ms();
    unsigned step_ms = app->game.score < 240
                           ? 180u - (unsigned)app->game.score / 3u
                           : 100u;
    if(app->game.state == SNAKE_STATE_RUNNING && now >= app->next_step) {
        snake_game_step(&app->game);
        app->next_step = now + step_ms;
    }

    if(!game_platform_acquire_frame(&app->platform, &framebuffer))
        return false;
    draw_scene(&framebuffer, &app->game);
    return game_platform_present(&app->platform);
}

int main(void)
{
    static snake_app_t app;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    if(!game_platform_init(&app.platform)) {
        fprintf(stderr, "snake: failed to initialize platform\n");
        return 1;
    }

    snake_game_init(&app.game, (uint32_t)game_monotonic_ms());
    game_input_set_repeat(&app.platform, 300, 120);
    app.next_step = game_monotonic_ms();

    game_run(&app.platform, tick, &app);

    game_platform_destroy(&app.platform);
    return 0;
}
