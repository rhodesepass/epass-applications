#include "logic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if(!(condition)) {                                                      \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while(0)

static void expect_merge(const uint32_t input[GAME_2048_SIZE],
                         const uint32_t expected[GAME_2048_SIZE],
                         uint32_t expected_gain, bool expected_changed)
{
    uint32_t line[GAME_2048_SIZE];
    uint32_t gain = UINT32_MAX;
    memcpy(line, input, sizeof(line));
    CHECK(game_2048_merge_line(line, &gain) == expected_changed);
    CHECK(memcmp(line, expected, sizeof(line)) == 0);
    CHECK(gain == expected_gain);
}

static void test_standard_merges(void)
{
    expect_merge((uint32_t[]){2, 2, 2, 2},
                 (uint32_t[]){4, 4, 0, 0}, 8, true);
    expect_merge((uint32_t[]){4, 4, 4, 0},
                 (uint32_t[]){8, 4, 0, 0}, 8, true);
    expect_merge((uint32_t[]){2, 2, 4, 0},
                 (uint32_t[]){4, 4, 0, 0}, 4, true);
    expect_merge((uint32_t[]){2, 0, 2, 2},
                 (uint32_t[]){4, 2, 0, 0}, 4, true);
    expect_merge((uint32_t[]){4, 4, 8, 8},
                 (uint32_t[]){8, 16, 0, 0}, 24, true);
    expect_merge((uint32_t[]){2, 4, 8, 16},
                 (uint32_t[]){2, 4, 8, 16}, 0, false);
}

static void test_move_detection(void)
{
    game_2048_t game = {
        .cells = {
            {2, 4, 2, 4},
            {4, 2, 4, 2},
            {2, 4, 2, 4},
            {4, 2, 4, 2}
        }
    };
    CHECK(!game_2048_can_move(&game));
    game.cells[3][3] = 0;
    CHECK(game_2048_can_move(&game));
    game.cells[3][3] = 4;
    CHECK(game_2048_can_move(&game));
}

static void test_board_move_scores_once(void)
{
    game_2048_t game = {
        .cells = {
            {2, 2, 2, 2},
            {0, 0, 0, 0},
            {0, 0, 0, 0},
            {0, 0, 0, 0}
        },
        .random_state = 1
    };
    CHECK(game_2048_move(&game, GAME_2048_LEFT));
    CHECK(game.cells[0][0] == 4);
    CHECK(game.cells[0][1] == 4);
    CHECK(game.score == 8);

    unsigned nonzero = 0;
    for(unsigned row = 0; row < GAME_2048_SIZE; row++)
        for(unsigned column = 0; column < GAME_2048_SIZE; column++)
            if(game.cells[row][column] != 0) nonzero++;
    CHECK(nonzero == 3);
}

int main(void)
{
    test_standard_merges();
    test_move_detection();
    test_board_move_scores_once();
    puts("game_2048 logic tests passed");
    return 0;
}
