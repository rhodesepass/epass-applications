#ifndef SNAKE_LOGIC_H
#define SNAKE_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SNAKE_GRID_WIDTH 16
#define SNAKE_GRID_HEIGHT 20
#define SNAKE_MAX_CELLS (SNAKE_GRID_WIDTH * SNAKE_GRID_HEIGHT)

typedef struct {
    int x;
    int y;
} snake_point_t;

typedef enum {
    SNAKE_DIRECTION_UP = 0,
    SNAKE_DIRECTION_RIGHT,
    SNAKE_DIRECTION_DOWN,
    SNAKE_DIRECTION_LEFT
} snake_direction_t;

typedef enum {
    SNAKE_STATE_READY = 0,
    SNAKE_STATE_RUNNING,
    SNAKE_STATE_PAUSED,
    SNAKE_STATE_GAME_OVER
} snake_state_t;

typedef struct {
    snake_point_t body[SNAKE_MAX_CELLS];
    size_t length;
    snake_point_t food;
    snake_direction_t direction;
    snake_state_t state;
    int score;
    uint32_t random_state;
    bool won;
} snake_game_t;

void snake_game_init(snake_game_t *game, uint32_t seed);
void snake_game_start(snake_game_t *game);
void snake_game_toggle_pause(snake_game_t *game);
void snake_game_turn_left(snake_game_t *game);
void snake_game_turn_right(snake_game_t *game);
bool snake_game_step(snake_game_t *game);
bool snake_game_occupies(const snake_game_t *game, int x, int y);

#endif
