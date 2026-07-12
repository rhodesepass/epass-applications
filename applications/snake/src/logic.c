#include "logic.h"

#include <string.h>

static uint32_t next_random(snake_game_t *game)
{
    uint32_t value = game->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game->random_state = value ? value : 0x6d2b79f5u;
    return game->random_state;
}

bool snake_game_occupies(const snake_game_t *game, int x, int y)
{
    if(!game) return false;
    for(size_t i = 0; i < game->length; ++i)
        if(game->body[i].x == x && game->body[i].y == y) return true;
    return false;
}

static void place_food(snake_game_t *game)
{
    size_t free_cells = SNAKE_MAX_CELLS - game->length;
    if(free_cells == 0) {
        game->won = true;
        game->state = SNAKE_STATE_GAME_OVER;
        return;
    }

    size_t target = next_random(game) % free_cells;
    for(int y = 0; y < SNAKE_GRID_HEIGHT; ++y) {
        for(int x = 0; x < SNAKE_GRID_WIDTH; ++x) {
            if(snake_game_occupies(game, x, y)) continue;
            if(target-- == 0) {
                game->food = (snake_point_t){x, y};
                return;
            }
        }
    }
}

void snake_game_init(snake_game_t *game, uint32_t seed)
{
    if(!game) return;
    memset(game, 0, sizeof(*game));
    game->random_state = seed ? seed : 0x6d2b79f5u;
    game->direction = SNAKE_DIRECTION_RIGHT;
    game->state = SNAKE_STATE_READY;
    game->length = 4;
    game->body[0] = (snake_point_t){8, 10};
    game->body[1] = (snake_point_t){7, 10};
    game->body[2] = (snake_point_t){6, 10};
    game->body[3] = (snake_point_t){5, 10};
    place_food(game);
}

void snake_game_start(snake_game_t *game)
{
    if(game && game->state == SNAKE_STATE_READY)
        game->state = SNAKE_STATE_RUNNING;
}

void snake_game_toggle_pause(snake_game_t *game)
{
    if(!game) return;
    if(game->state == SNAKE_STATE_RUNNING)
        game->state = SNAKE_STATE_PAUSED;
    else if(game->state == SNAKE_STATE_PAUSED)
        game->state = SNAKE_STATE_RUNNING;
}

void snake_game_turn_left(snake_game_t *game)
{
    if(game && (game->state == SNAKE_STATE_RUNNING ||
                game->state == SNAKE_STATE_PAUSED))
        game->direction = (snake_direction_t)((game->direction + 3) % 4);
}

void snake_game_turn_right(snake_game_t *game)
{
    if(game && (game->state == SNAKE_STATE_RUNNING ||
                game->state == SNAKE_STATE_PAUSED))
        game->direction = (snake_direction_t)((game->direction + 1) % 4);
}

bool snake_game_step(snake_game_t *game)
{
    static const int delta_x[] = {0, 1, 0, -1};
    static const int delta_y[] = {-1, 0, 1, 0};
    if(!game || game->state != SNAKE_STATE_RUNNING) return false;

    snake_point_t head = game->body[0];
    head.x += delta_x[game->direction];
    head.y += delta_y[game->direction];
    bool eating = head.x == game->food.x && head.y == game->food.y;

    if(head.x < 0 || head.x >= SNAKE_GRID_WIDTH ||
       head.y < 0 || head.y >= SNAKE_GRID_HEIGHT) {
        game->state = SNAKE_STATE_GAME_OVER;
        return false;
    }

    size_t collision_length = game->length - (eating ? 0u : 1u);
    for(size_t i = 0; i < collision_length; ++i) {
        if(game->body[i].x == head.x && game->body[i].y == head.y) {
            game->state = SNAKE_STATE_GAME_OVER;
            return false;
        }
    }

    size_t moved = game->length;
    if(eating && game->length < SNAKE_MAX_CELLS) ++moved;
    memmove(&game->body[1], &game->body[0],
            (moved - 1) * sizeof(game->body[0]));
    game->body[0] = head;
    game->length = moved;

    if(eating) {
        game->score += 10;
        place_food(game);
    }
    return true;
}
