#include "epass_game.h"
#include "logic.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define BOARD_X 18
#define BOARD_Y 66
#define CELL_SIZE 22

static volatile sig_atomic_t running = 1;

static const uint32_t piece_colors[TETROMINO_COUNT] = {
    0xff35c9e8, 0xff3976d6, 0xffff9838, 0xffffd43b,
    0xff4ecb71, 0xffa761d1, 0xffed5267
};

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void draw_cell(game_framebuffer_t *fb, int board_x, int board_y,
                      uint32_t color)
{
    int x = BOARD_X + board_x * CELL_SIZE;
    int y = BOARD_Y + board_y * CELL_SIZE;
    game_draw_rect(fb, x + 1, y + 1, CELL_SIZE - 2, CELL_SIZE - 2, color);
    game_draw_rect(fb, x + 3, y + 3, CELL_SIZE - 6, 3, 0x66ffffff);
}

static void draw_next_piece(game_framebuffer_t *fb, tetromino_t type)
{
    game_draw_text(fb, 253, 202, "NEXT", 2, 0xffb9c7da);
    for(int y = 0; y < 4; y++)
        for(int x = 0; x < 4; x++)
            if(tetris_piece_has_cell(type, 0, x, y))
                game_draw_rect(fb, 254 + x * 14, 226 + y * 14,
                               12, 12, piece_colors[type]);
}

static void draw_game(game_framebuffer_t *fb, const tetris_game_t *game)
{
    game_draw_fill(fb, 0xff09111f);
    game_draw_text(fb, 18, 18, "TETRIS", 4, 0xff64d8ff);

    game_draw_rect(fb, BOARD_X - 3, BOARD_Y - 3,
                   TETRIS_BOARD_WIDTH * CELL_SIZE + 6,
                   TETRIS_BOARD_HEIGHT * CELL_SIZE + 6, 0xff6c809b);
    game_draw_rect(fb, BOARD_X, BOARD_Y,
                   TETRIS_BOARD_WIDTH * CELL_SIZE,
                   TETRIS_BOARD_HEIGHT * CELL_SIZE, 0xff111d30);

    for(int y = 0; y < TETRIS_BOARD_HEIGHT; y++) {
        for(int x = 0; x < TETRIS_BOARD_WIDTH; x++) {
            uint8_t cell = game->board[y][x];
            if(cell > 0)
                draw_cell(fb, x, y, piece_colors[cell - 1]);
            else if(tetris_active_has_cell(game, x, y))
                draw_cell(fb, x, y, piece_colors[game->active.type]);
        }
    }

    game_draw_text(fb, 253, 76, "SCORE", 2, 0xffb9c7da);
    game_draw_number(fb, 253, 98, game->score, 2, 0xffffffff);
    game_draw_text(fb, 253, 128, "LINES", 2, 0xffb9c7da);
    game_draw_number(fb, 253, 150, game->lines, 2, 0xffffffff);
    game_draw_text(fb, 253, 174, "LEVEL", 1, 0xffb9c7da);
    game_draw_number(fb, 303, 174, game->level, 1, 0xffffffff);
    draw_next_piece(fb, game->next);

    game_draw_text(fb, 248, 326, "KEY 1", 1, 0xff64d8ff);
    game_draw_text(fb, 248, 338, "LEFT", 1, 0xffffffff);
    game_draw_text(fb, 248, 358, "KEY 2", 1, 0xff64d8ff);
    game_draw_text(fb, 248, 370, "RIGHT", 1, 0xffffffff);
    game_draw_text(fb, 248, 390, "KEY 3", 1, 0xff64d8ff);
    game_draw_text(fb, 248, 402, "ROTATE", 1, 0xffffffff);
    game_draw_text(fb, 248, 414, "HOLD DROP", 1, 0xffffffff);
    game_draw_text(fb, 248, 438, "KEY 4", 1, 0xff64d8ff);
    game_draw_text(fb, 248, 450, "EXIT", 1, 0xffffffff);

    if(game->game_over) {
        game_draw_rect(fb, 38, 244, 180, 112, 0xee09111f);
        game_draw_rect(fb, 42, 248, 172, 104, 0xff263b57);
        game_draw_text(fb, 55, 270, "GAME OVER", 3, 0xffff6b76);
        game_draw_text(fb, 62, 314, "KEY 3 RESTART", 1, 0xffffffff);
    }
}

typedef struct {
    game_platform_t platform;
    tetris_game_t game;
    uint64_t previous_ms;
    uint64_t key3_held_since;
    uint64_t next_soft_drop;
} tetris_app_t;

static bool tick(void *userdata)
{
    tetris_app_t *app = userdata;

    if(!running) return false;
    uint64_t now = game_monotonic_ms();
    uint32_t elapsed = (uint32_t)(now - app->previous_ms);
    app->previous_ms = now;
    if(elapsed > 100) elapsed = 100;

    game_input_update(&app->platform);
    if(game_key_pressed(&app->platform, GAME_KEY_BACK))
        return false;

    if(app->game.game_over) {
        if(game_key_pressed(&app->platform, GAME_KEY_OK))
            tetris_init(&app->game, (uint32_t)(now ^ (uint64_t)getpid()));
    } else {
        if(game_key_pressed(&app->platform, GAME_KEY_UP) ||
           game_key_repeated(&app->platform, GAME_KEY_UP))
            tetris_move(&app->game, -1);
        if(game_key_pressed(&app->platform, GAME_KEY_DOWN) ||
           game_key_repeated(&app->platform, GAME_KEY_DOWN))
            tetris_move(&app->game, 1);

        if(game_key_pressed(&app->platform, GAME_KEY_OK)) {
            tetris_rotate(&app->game);
            app->key3_held_since = now;
            app->next_soft_drop = now + 300;
        }
        if(game_key_down(&app->platform, GAME_KEY_OK) &&
           app->key3_held_since != 0 && now >= app->next_soft_drop) {
            tetris_soft_drop(&app->game);
            app->next_soft_drop = now + 55;
        }
        if(!game_key_down(&app->platform, GAME_KEY_OK))
            app->key3_held_since = 0;
        tetris_tick(&app->game, elapsed);
    }

    game_framebuffer_t framebuffer;
    if(!game_platform_acquire_frame(&app->platform, &framebuffer))
        return false;
    draw_game(&framebuffer, &app->game);
    if(!game_platform_present(&app->platform))
        return false;
    game_platform_idle(&app->platform, 8);
    return true;
}

int main(void)
{
    static tetris_app_t app;

    signal(SIGINT, stop_running);
    signal(SIGTERM, stop_running);
    if(!game_platform_init(&app.platform)) {
        fprintf(stderr, "tetris: platform initialization failed\n");
        return 1;
    }

    game_input_set_repeat(&app.platform, 260, 75);
    app.previous_ms = game_monotonic_ms();
    tetris_init(&app.game, (uint32_t)(app.previous_ms ^ (uint64_t)getpid()));

    game_run(&app.platform, tick, &app);

    game_platform_destroy(&app.platform);
    return 0;
}
