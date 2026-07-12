#ifndef GAME_2048_LOGIC_H
#define GAME_2048_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#define GAME_2048_SIZE 4
#define GAME_2048_WIN_TILE 2048

typedef enum {
    GAME_2048_UP = 0,
    GAME_2048_RIGHT,
    GAME_2048_DOWN,
    GAME_2048_LEFT,
    GAME_2048_DIRECTION_COUNT
} game_2048_direction_t;

typedef struct {
    uint32_t cells[GAME_2048_SIZE][GAME_2048_SIZE];
    uint32_t score;
    uint32_t random_state;
    bool won;
} game_2048_t;

void game_2048_reset(game_2048_t *game, uint32_t seed);
bool game_2048_move(game_2048_t *game, game_2048_direction_t direction);
bool game_2048_can_move(const game_2048_t *game);

/* Pure line operation used by the board mover and host-side rule tests. */
bool game_2048_merge_line(uint32_t line[GAME_2048_SIZE],
                          uint32_t *score_gain);

#endif
