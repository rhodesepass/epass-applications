#include "epass_game.h"

#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#define GRAVITY 0.5f
#define JUMP_STRENGTH (-8.0f)
#define PIPE_SPEED 4.0f
#define PIPE_WIDTH 60
#define PIPE_GAP 180
#define BIRD_X 80
#define BIRD_SIZE 30
#define MAX_PIPES 3

typedef struct {
    float y;
    float velocity;
} bird_t;

typedef struct {
    float x;
    float gap_y;
    bool passed;
} pipe_t;

typedef enum {
    STATE_START,
    STATE_PLAYING,
    STATE_GAME_OVER
} game_state_t;

static volatile sig_atomic_t running = 1;
static bird_t bird;
static pipe_t pipes[MAX_PIPES];
static int score;
static game_state_t state = STATE_START;

static void handle_signal(int signo)
{
    (void)signo;
    running = 0;
}

static void reset_game(void)
{
    bird.y = GAME_LOGICAL_HEIGHT / 2.0f;
    bird.velocity = 0.0f;
    score = 0;
    for(int i = 0; i < MAX_PIPES; i++) {
        pipes[i].x = GAME_LOGICAL_WIDTH +
                    i * (GAME_LOGICAL_WIDTH / 2 + PIPE_WIDTH);
        pipes[i].gap_y = 150 + rand() % (GAME_LOGICAL_HEIGHT - 300);
        pipes[i].passed = false;
    }
}

static void jump(void)
{
    if(state == STATE_GAME_OVER) {
        reset_game();
        state = STATE_START;
        return;
    }
    if(state == STATE_START) state = STATE_PLAYING;
    bird.velocity = JUMP_STRENGTH;
}

static void update_game(void)
{
    if(state != STATE_PLAYING) return;

    bird.velocity += GRAVITY;
    bird.y += bird.velocity;
    if(bird.y < 0.0f) {
        bird.y = 0.0f;
        bird.velocity = 0.0f;
    }
    if(bird.y + BIRD_SIZE > GAME_LOGICAL_HEIGHT)
        state = STATE_GAME_OVER;

    for(int i = 0; i < MAX_PIPES; i++) {
        pipes[i].x -= PIPE_SPEED;
        if(pipes[i].x + PIPE_WIDTH < 0.0f) {
            pipes[i].x += MAX_PIPES *
                          (GAME_LOGICAL_WIDTH / 2 + PIPE_WIDTH);
            pipes[i].gap_y =
                150 + rand() % (GAME_LOGICAL_HEIGHT - 300);
            pipes[i].passed = false;
        }
        if(BIRD_X + BIRD_SIZE > pipes[i].x &&
           BIRD_X < pipes[i].x + PIPE_WIDTH &&
           (bird.y < pipes[i].gap_y - PIPE_GAP / 2 ||
            bird.y + BIRD_SIZE > pipes[i].gap_y + PIPE_GAP / 2))
            state = STATE_GAME_OVER;

        if(!pipes[i].passed && pipes[i].x + PIPE_WIDTH < BIRD_X) {
            pipes[i].passed = true;
            score++;
        }
    }
}

static void draw_game(game_framebuffer_t *fb)
{
    game_draw_fill(fb, 0xff87ceeb);
    for(int i = 0; i < MAX_PIPES; i++) {
        int x = (int)pipes[i].x;
        int gap_y = (int)pipes[i].gap_y;
        game_draw_rect(fb, x, 0, PIPE_WIDTH, gap_y - PIPE_GAP / 2,
                       0xff228b22);
        game_draw_rect(fb, x, gap_y + PIPE_GAP / 2, PIPE_WIDTH,
                       GAME_LOGICAL_HEIGHT - gap_y - PIPE_GAP / 2,
                       0xff228b22);
    }
    game_draw_rect(fb, BIRD_X, (int)bird.y, BIRD_SIZE, BIRD_SIZE,
                   0xffffff00);

    if(state == STATE_START) {
        game_draw_text(fb, 72, 250, "FLAPPY BIRD", 4, 0xffffffff);
        game_draw_text(fb, 57, 300, "KEY_3/KEY_1: JUMP", 2, 0xffffffff);
    } else if(state == STATE_PLAYING) {
        game_draw_text(fb, 10, 10, "SCORE:", 3, 0xffffffff);
        game_draw_number(fb, 118, 10, score, 3, 0xffffffff);
    } else {
        game_draw_text(fb, 72, 250, "GAME OVER", 4, 0xffffffff);
        game_draw_text(fb, 102, 300, "SCORE:", 3, 0xffffffff);
        game_draw_number(fb, 210, 300, score, 3, 0xffffffff);
        game_draw_text(fb, 51, 350, "KEY_3: RESTART", 3, 0xffffffff);
    }
}

int main(void)
{
    game_platform_t platform = {0};
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    srand((unsigned int)time(NULL));
    reset_game();
    if(!game_platform_init(&platform)) return 1;

    while(running) {
        game_input_update(&platform);
        if(game_key_pressed(&platform, GAME_KEY_BACK)) break;
        if(game_key_pressed(&platform, GAME_KEY_OK) ||
           game_key_pressed(&platform, GAME_KEY_UP))
            jump();

        update_game();
        game_framebuffer_t fb;
        if(!game_platform_acquire_frame(&platform, &fb)) break;
        draw_game(&fb);
        if(!game_platform_present(&platform)) break;
    }

    game_platform_destroy(&platform);
    return 0;
}
