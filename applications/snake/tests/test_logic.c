#include "logic.h"

#include <assert.h>
#include <stdio.h>

static void test_start_turn_and_pause(void)
{
    snake_game_t game;
    snake_game_init(&game, 1);
    assert(game.state == SNAKE_STATE_READY);
    assert(game.length == 4);

    snake_game_start(&game);
    snake_game_turn_left(&game);
    assert(game.direction == SNAKE_DIRECTION_UP);
    snake_game_turn_right(&game);
    assert(game.direction == SNAKE_DIRECTION_RIGHT);

    snake_game_toggle_pause(&game);
    snake_point_t head = game.body[0];
    assert(!snake_game_step(&game));
    assert(game.body[0].x == head.x && game.body[0].y == head.y);
    snake_game_toggle_pause(&game);
    assert(game.state == SNAKE_STATE_RUNNING);
}

static void test_growth_and_score(void)
{
    snake_game_t game;
    snake_game_init(&game, 2);
    snake_game_start(&game);
    game.food = (snake_point_t){game.body[0].x + 1, game.body[0].y};

    assert(snake_game_step(&game));
    assert(game.length == 5);
    assert(game.score == 10);
    assert(!snake_game_occupies(&game, game.food.x, game.food.y));
}

static void test_boundary_collision(void)
{
    snake_game_t game;
    snake_game_init(&game, 3);
    snake_game_start(&game);
    game.length = 1;
    game.body[0] = (snake_point_t){0, 0};
    game.direction = SNAKE_DIRECTION_UP;
    game.food = (snake_point_t){5, 5};

    assert(!snake_game_step(&game));
    assert(game.state == SNAKE_STATE_GAME_OVER);
}

static void test_self_collision(void)
{
    snake_game_t game;
    snake_game_init(&game, 4);
    snake_game_start(&game);
    game.length = 5;
    game.body[0] = (snake_point_t){2, 2};
    game.body[1] = (snake_point_t){1, 2};
    game.body[2] = (snake_point_t){1, 3};
    game.body[3] = (snake_point_t){2, 3};
    game.body[4] = (snake_point_t){3, 3};
    game.direction = SNAKE_DIRECTION_DOWN;
    game.food = (snake_point_t){8, 8};

    assert(!snake_game_step(&game));
    assert(game.state == SNAKE_STATE_GAME_OVER);
}

int main(void)
{
    test_start_turn_and_pause();
    test_growth_and_score();
    test_boundary_collision();
    test_self_collision();
    puts("snake logic tests passed");
    return 0;
}
