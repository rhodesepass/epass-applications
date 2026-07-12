#ifndef TETRIS_LOGIC_H
#define TETRIS_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define TETRIS_BOARD_WIDTH 10
#define TETRIS_BOARD_HEIGHT 20

typedef enum {
    TETROMINO_I = 0,
    TETROMINO_J,
    TETROMINO_L,
    TETROMINO_O,
    TETROMINO_S,
    TETROMINO_T,
    TETROMINO_Z,
    TETROMINO_COUNT
} tetromino_t;

typedef struct {
    tetromino_t type;
    int rotation;
    int x;
    int y;
} tetris_piece_t;

typedef struct {
    uint8_t board[TETRIS_BOARD_HEIGHT][TETRIS_BOARD_WIDTH];
    tetris_piece_t active;
    tetromino_t next;
    uint32_t random_state;
    tetromino_t bag[TETROMINO_COUNT];
    int bag_index;
    uint32_t fall_accumulator_ms;
    uint32_t lock_accumulator_ms;
    int score;
    int lines;
    int level;
    bool game_over;
} tetris_game_t;

void tetris_init(tetris_game_t *game, uint32_t seed);
bool tetris_move(tetris_game_t *game, int dx);
bool tetris_rotate(tetris_game_t *game);
bool tetris_soft_drop(tetris_game_t *game);
void tetris_tick(tetris_game_t *game, uint32_t elapsed_ms);

bool tetris_piece_has_cell(tetromino_t type, int rotation, int x, int y);
bool tetris_active_has_cell(const tetris_game_t *game, int board_x, int board_y);
int tetris_clear_lines(tetris_game_t *game);
uint32_t tetris_drop_interval_ms(const tetris_game_t *game);

#endif
