#include "logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_all_tetrominoes_have_four_cells(void)
{
    for(int type = 0; type < TETROMINO_COUNT; type++) {
        for(int rotation = 0; rotation < 4; rotation++) {
            int cells = 0;
            for(int y = 0; y < 4; y++)
                for(int x = 0; x < 4; x++)
                    cells += tetris_piece_has_cell((tetromino_t)type,
                                                   rotation, x, y);
            assert(cells == 4);
        }
    }
}

static void test_rotation_wall_kick(void)
{
    tetris_game_t game;
    tetris_init(&game, 1);
    memset(game.board, 0, sizeof(game.board));
    game.active = (tetris_piece_t){
        .type = TETROMINO_I, .rotation = 1, .x = 7, .y = 4
    };

    assert(tetris_rotate(&game));
    assert(game.active.rotation == 2);
    assert(game.active.x == 6);
}

static void test_clear_multiple_lines_and_compact(void)
{
    tetris_game_t game;
    tetris_init(&game, 2);
    memset(game.board, 0, sizeof(game.board));
    for(int x = 0; x < TETRIS_BOARD_WIDTH; x++) {
        game.board[18][x] = 1;
        game.board[19][x] = 2;
    }
    game.board[17][3] = 7;

    assert(tetris_clear_lines(&game) == 2);
    assert(game.board[19][3] == 7);
    for(int x = 0; x < TETRIS_BOARD_WIDTH; x++)
        assert(game.board[18][x] == 0);
}

int main(void)
{
    test_all_tetrominoes_have_four_cells();
    test_rotation_wall_kick();
    test_clear_multiple_lines_and_compact();
    puts("tetris logic tests passed");
    return 0;
}
