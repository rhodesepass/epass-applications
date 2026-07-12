#include "logic.h"

#include <stddef.h>
#include <string.h>

static uint32_t next_random(game_2048_t *game)
{
    uint32_t value = game->random_state;
    if(value == 0) value = 0x6d2b79f5u;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    game->random_state = value;
    return value;
}

static void add_random_tile(game_2048_t *game)
{
    unsigned empty_count = 0;
    for(unsigned row = 0; row < GAME_2048_SIZE; row++)
        for(unsigned column = 0; column < GAME_2048_SIZE; column++)
            if(game->cells[row][column] == 0) empty_count++;

    if(empty_count == 0) return;

    unsigned target = next_random(game) % empty_count;
    uint32_t value = next_random(game) % 10 == 0 ? 4u : 2u;
    for(unsigned row = 0; row < GAME_2048_SIZE; row++) {
        for(unsigned column = 0; column < GAME_2048_SIZE; column++) {
            if(game->cells[row][column] != 0) continue;
            if(target-- == 0) {
                game->cells[row][column] = value;
                return;
            }
        }
    }
}

void game_2048_reset(game_2048_t *game, uint32_t seed)
{
    if(!game) return;
    memset(game, 0, sizeof(*game));
    game->random_state = seed ? seed : 0x6d2b79f5u;
    add_random_tile(game);
    add_random_tile(game);
}

bool game_2048_merge_line(uint32_t line[GAME_2048_SIZE],
                          uint32_t *score_gain)
{
    uint32_t compacted[GAME_2048_SIZE] = {0};
    uint32_t result[GAME_2048_SIZE] = {0};
    unsigned compacted_count = 0;
    unsigned result_count = 0;
    uint32_t gain = 0;

    if(!line) return false;
    for(unsigned i = 0; i < GAME_2048_SIZE; i++)
        if(line[i] != 0) compacted[compacted_count++] = line[i];

    for(unsigned i = 0; i < compacted_count; i++) {
        if(i + 1 < compacted_count && compacted[i] == compacted[i + 1]) {
            result[result_count] = compacted[i] * 2;
            gain += result[result_count++];
            i++;
        } else {
            result[result_count++] = compacted[i];
        }
    }

    bool changed = memcmp(line, result, sizeof(result)) != 0;
    memcpy(line, result, sizeof(result));
    if(score_gain) *score_gain = gain;
    return changed;
}

static void read_line(const game_2048_t *game, game_2048_direction_t direction,
                      unsigned index, uint32_t line[GAME_2048_SIZE])
{
    for(unsigned offset = 0; offset < GAME_2048_SIZE; offset++) {
        switch(direction) {
        case GAME_2048_UP:
            line[offset] = game->cells[offset][index];
            break;
        case GAME_2048_RIGHT:
            line[offset] = game->cells[index][GAME_2048_SIZE - 1 - offset];
            break;
        case GAME_2048_DOWN:
            line[offset] = game->cells[GAME_2048_SIZE - 1 - offset][index];
            break;
        case GAME_2048_LEFT:
            line[offset] = game->cells[index][offset];
            break;
        default:
            line[offset] = 0;
            break;
        }
    }
}

static void write_line(game_2048_t *game, game_2048_direction_t direction,
                       unsigned index,
                       const uint32_t line[GAME_2048_SIZE])
{
    for(unsigned offset = 0; offset < GAME_2048_SIZE; offset++) {
        switch(direction) {
        case GAME_2048_UP:
            game->cells[offset][index] = line[offset];
            break;
        case GAME_2048_RIGHT:
            game->cells[index][GAME_2048_SIZE - 1 - offset] = line[offset];
            break;
        case GAME_2048_DOWN:
            game->cells[GAME_2048_SIZE - 1 - offset][index] = line[offset];
            break;
        case GAME_2048_LEFT:
            game->cells[index][offset] = line[offset];
            break;
        default:
            break;
        }
    }
}

bool game_2048_move(game_2048_t *game, game_2048_direction_t direction)
{
    bool changed = false;
    uint32_t total_gain = 0;

    if(!game || direction < 0 || direction >= GAME_2048_DIRECTION_COUNT)
        return false;

    for(unsigned index = 0; index < GAME_2048_SIZE; index++) {
        uint32_t line[GAME_2048_SIZE];
        uint32_t gain = 0;
        read_line(game, direction, index, line);
        if(game_2048_merge_line(line, &gain)) changed = true;
        write_line(game, direction, index, line);
        total_gain += gain;
    }

    if(!changed) return false;
    game->score += total_gain;
    for(unsigned row = 0; row < GAME_2048_SIZE; row++)
        for(unsigned column = 0; column < GAME_2048_SIZE; column++)
            if(game->cells[row][column] >= GAME_2048_WIN_TILE)
                game->won = true;
    add_random_tile(game);
    return true;
}

bool game_2048_can_move(const game_2048_t *game)
{
    if(!game) return false;
    for(unsigned row = 0; row < GAME_2048_SIZE; row++) {
        for(unsigned column = 0; column < GAME_2048_SIZE; column++) {
            uint32_t value = game->cells[row][column];
            if(value == 0) return true;
            if(column + 1 < GAME_2048_SIZE &&
               value == game->cells[row][column + 1])
                return true;
            if(row + 1 < GAME_2048_SIZE &&
               value == game->cells[row + 1][column])
                return true;
        }
    }
    return false;
}
