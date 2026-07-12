#include "logic.h"

#include <string.h>

#define LOCK_DELAY_MS 500u

/* Four 4x4 bitmaps for each of the seven SRS-style tetrominoes. */
static const uint16_t shapes[TETROMINO_COUNT][4] = {
    {0x00f0, 0x4444, 0x0f00, 0x2222}, /* I */
    {0x0071, 0x0226, 0x0470, 0x0322}, /* J */
    {0x0074, 0x0622, 0x0170, 0x0223}, /* L */
    {0x0066, 0x0066, 0x0066, 0x0066}, /* O */
    {0x0036, 0x0462, 0x0360, 0x0231}, /* S */
    {0x0072, 0x0262, 0x0270, 0x0232}, /* T */
    {0x0063, 0x0264, 0x0630, 0x0132}  /* Z */
};

static uint32_t next_random(tetris_game_t *game)
{
    uint32_t x = game->random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->random_state = x;
    return x;
}

static tetromino_t take_from_bag(tetris_game_t *game)
{
    if(game->bag_index >= TETROMINO_COUNT) {
        for(int i = 0; i < TETROMINO_COUNT; i++)
            game->bag[i] = (tetromino_t)i;
        for(int i = TETROMINO_COUNT - 1; i > 0; i--) {
            int j = (int)(next_random(game) % (uint32_t)(i + 1));
            tetromino_t temporary = game->bag[i];
            game->bag[i] = game->bag[j];
            game->bag[j] = temporary;
        }
        game->bag_index = 0;
    }
    return game->bag[game->bag_index++];
}

bool tetris_piece_has_cell(tetromino_t type, int rotation, int x, int y)
{
    if(type < 0 || type >= TETROMINO_COUNT ||
       x < 0 || x >= 4 || y < 0 || y >= 4)
        return false;
    return (shapes[type][rotation & 3] & (1u << (y * 4 + x))) != 0;
}

static bool piece_fits(const tetris_game_t *game, const tetris_piece_t *piece)
{
    for(int py = 0; py < 4; py++) {
        for(int px = 0; px < 4; px++) {
            if(!tetris_piece_has_cell(piece->type, piece->rotation, px, py))
                continue;
            int x = piece->x + px;
            int y = piece->y + py;
            if(x < 0 || x >= TETRIS_BOARD_WIDTH || y >= TETRIS_BOARD_HEIGHT)
                return false;
            if(y >= 0 && game->board[y][x] != 0)
                return false;
        }
    }
    return true;
}

static void spawn_piece(tetris_game_t *game)
{
    game->active = (tetris_piece_t){
        .type = game->next, .rotation = 0, .x = 3, .y = -1
    };
    game->next = take_from_bag(game);
    game->fall_accumulator_ms = 0;
    game->lock_accumulator_ms = 0;
    if(!piece_fits(game, &game->active))
        game->game_over = true;
}

void tetris_init(tetris_game_t *game, uint32_t seed)
{
    memset(game, 0, sizeof(*game));
    game->random_state = seed ? seed : 0x6d2b79f5u;
    game->bag_index = TETROMINO_COUNT;
    game->level = 1;
    game->next = take_from_bag(game);
    spawn_piece(game);
}

bool tetris_move(tetris_game_t *game, int dx)
{
    if(!game || game->game_over || dx == 0)
        return false;
    tetris_piece_t moved = game->active;
    moved.x += dx;
    if(!piece_fits(game, &moved))
        return false;
    game->active = moved;
    game->lock_accumulator_ms = 0;
    return true;
}

bool tetris_rotate(tetris_game_t *game)
{
    static const int kicks[][2] = {
        {0, 0}, {-1, 0}, {1, 0}, {-2, 0}, {2, 0}, {0, -1}
    };
    if(!game || game->game_over)
        return false;
    tetris_piece_t rotated = game->active;
    rotated.rotation = (rotated.rotation + 1) & 3;
    for(unsigned int i = 0; i < sizeof(kicks) / sizeof(kicks[0]); i++) {
        rotated.x = game->active.x + kicks[i][0];
        rotated.y = game->active.y + kicks[i][1];
        if(piece_fits(game, &rotated)) {
            game->active = rotated;
            game->lock_accumulator_ms = 0;
            return true;
        }
    }
    return false;
}

int tetris_clear_lines(tetris_game_t *game)
{
    int write_y = TETRIS_BOARD_HEIGHT - 1;
    int cleared = 0;
    for(int read_y = TETRIS_BOARD_HEIGHT - 1; read_y >= 0; read_y--) {
        bool full = true;
        for(int x = 0; x < TETRIS_BOARD_WIDTH; x++)
            if(game->board[read_y][x] == 0) full = false;
        if(full) {
            cleared++;
            continue;
        }
        if(write_y != read_y)
            memcpy(game->board[write_y], game->board[read_y],
                   sizeof(game->board[write_y]));
        write_y--;
    }
    while(write_y >= 0)
        memset(game->board[write_y--], 0, sizeof(game->board[0]));
    return cleared;
}

static void lock_piece(tetris_game_t *game)
{
    for(int py = 0; py < 4; py++) {
        for(int px = 0; px < 4; px++) {
            if(!tetris_piece_has_cell(game->active.type,
                                      game->active.rotation, px, py))
                continue;
            int x = game->active.x + px;
            int y = game->active.y + py;
            if(y < 0) {
                game->game_over = true;
                return;
            }
            game->board[y][x] = (uint8_t)game->active.type + 1;
        }
    }
    int cleared = tetris_clear_lines(game);
    static const int line_points[] = {0, 100, 300, 500, 800};
    game->score += line_points[cleared] * game->level;
    game->lines += cleared;
    game->level = game->lines / 10 + 1;
    spawn_piece(game);
}

static bool step_down(tetris_game_t *game)
{
    tetris_piece_t moved = game->active;
    moved.y++;
    if(!piece_fits(game, &moved))
        return false;
    game->active = moved;
    game->lock_accumulator_ms = 0;
    return true;
}

bool tetris_soft_drop(tetris_game_t *game)
{
    if(!game || game->game_over || !step_down(game))
        return false;
    game->score++;
    game->fall_accumulator_ms = 0;
    return true;
}

uint32_t tetris_drop_interval_ms(const tetris_game_t *game)
{
    int level = game ? game->level : 1;
    int interval = 760 - (level - 1) * 55;
    return (uint32_t)(interval < 90 ? 90 : interval);
}

void tetris_tick(tetris_game_t *game, uint32_t elapsed_ms)
{
    if(!game || game->game_over)
        return;
    game->fall_accumulator_ms += elapsed_ms;
    uint32_t interval = tetris_drop_interval_ms(game);
    while(game->fall_accumulator_ms >= interval) {
        game->fall_accumulator_ms -= interval;
        if(!step_down(game))
            break;
    }

    tetris_piece_t below = game->active;
    below.y++;
    if(piece_fits(game, &below)) {
        game->lock_accumulator_ms = 0;
    } else {
        game->lock_accumulator_ms += elapsed_ms;
        if(game->lock_accumulator_ms >= LOCK_DELAY_MS)
            lock_piece(game);
    }
}

bool tetris_active_has_cell(const tetris_game_t *game, int board_x, int board_y)
{
    if(!game || game->game_over)
        return false;
    int x = board_x - game->active.x;
    int y = board_y - game->active.y;
    return tetris_piece_has_cell(game->active.type, game->active.rotation, x, y);
}
