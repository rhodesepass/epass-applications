/*
 * epass_game 的 wasm/浏览器后端: canvas 2D 当显示层, window 键盘事件当按键。
 * 与 epass 后端(epass_game.c)二选一参与编译, 类比同一驱动接口的两份驱动。
 */
#include "../include/epass_game.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <stdlib.h>
#include <string.h>

/* 浏览器端直接用逻辑分辨率, 1:1 像素最清晰 */
#define WASM_WIDTH GAME_LOGICAL_WIDTH
#define WASM_HEIGHT GAME_LOGICAL_HEIGHT
#define WASM_EVENT_QUEUE 32

typedef struct {
    game_key_t key;
    bool pressed;
} wasm_key_event_t;

typedef struct {
    uint32_t pixels[WASM_WIDTH * WASM_HEIGHT]; /* ARGB8888, 游戏画在这 */
    uint32_t rgba[WASM_WIDTH * WASM_HEIGHT];   /* canvas 字节序, present 时转换 */
    bool acquired;
    /* 单线程, 无并发: 键盘回调与 tick 都在浏览器主线程 */
    wasm_key_event_t events[WASM_EVENT_QUEUE];
    int event_head, event_tail;
    bool down[GAME_KEY_COUNT];
    bool pressed[GAME_KEY_COUNT];
    bool repeated[GAME_KEY_COUNT];
    uint64_t next_repeat[GAME_KEY_COUNT];
    uint32_t repeat_delay, repeat_interval;
} game_platform_impl_t;

static game_platform_impl_t *get_impl(const game_platform_t *platform)
{
    return platform ? (game_platform_impl_t *)platform->impl : NULL;
}

/* game_monotonic_ms 用共享层 game_draw.c 的 clock_gettime 版,
 * emscripten 会映射到 performance.now */

/* 别名表与 hal_input 一致: 1/↑/← 2/↓/→ 3/Enter 4/Esc */
static int map_code(const char *code)
{
    if(!strcmp(code, "Digit1") || !strcmp(code, "ArrowUp") ||
       !strcmp(code, "ArrowLeft")) return GAME_KEY_UP;
    if(!strcmp(code, "Digit2") || !strcmp(code, "ArrowDown") ||
       !strcmp(code, "ArrowRight")) return GAME_KEY_DOWN;
    if(!strcmp(code, "Digit3") || !strcmp(code, "Enter")) return GAME_KEY_OK;
    if(!strcmp(code, "Digit4") || !strcmp(code, "Escape")) return GAME_KEY_BACK;
    return -1;
}

static EM_BOOL key_callback(int type, const EmscriptenKeyboardEvent *ev,
                         void *userdata)
{
    game_platform_impl_t *impl = userdata;
    int key = map_code(ev->code);
    if(key < 0) return EM_FALSE; /* 不认识的键还给浏览器 */
    /* 浏览器 autorepeat 不进队列, 重复节奏由帧级状态机掌控(与 epass 一致) */
    if(type == EMSCRIPTEN_EVENT_KEYDOWN && ev->repeat) return EM_TRUE;
    int next = (impl->event_tail + 1) % WASM_EVENT_QUEUE;
    if(next == impl->event_head) return EM_TRUE; /* 队满丢弃, 好过卡死 */
    impl->events[impl->event_tail].key = (game_key_t)key;
    impl->events[impl->event_tail].pressed = type == EMSCRIPTEN_EVENT_KEYDOWN;
    impl->event_tail = next;
    return EM_TRUE; /* 吃掉, 防页面滚动 */
}

bool game_platform_init(game_platform_t *platform)
{
    return game_platform_init_ex(platform, GAME_PIXEL_FORMAT_ARGB8888);
}

bool game_platform_init_ex(game_platform_t *platform,
                           game_pixel_format_t format)
{
    (void)format; /* canvas 只有 32 位一种, RGB565 是 epass 的省带宽手段 */
    game_platform_impl_t *impl;
    if(!platform || platform->impl) return false;
    impl = calloc(1, sizeof(*impl));
    if(!impl) return false;
    platform->impl = impl;
    impl->repeat_delay = 350;
    impl->repeat_interval = 90;

    EM_ASM({
        var canvas = document.getElementById('canvas');
        canvas.width = $0;
        canvas.height = $1;
        Module.epassCtx = canvas.getContext('2d');
    }, WASM_WIDTH, WASM_HEIGHT);

    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl,
                                    false, key_callback);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, impl,
                                  false, key_callback);
    return true;
}

bool game_platform_acquire_frame(game_platform_t *platform,
                                 game_framebuffer_t *framebuffer)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl || !framebuffer || impl->acquired) return false;
    impl->acquired = true;
    framebuffer->pixels = impl->pixels;
    framebuffer->width = WASM_WIDTH;
    framebuffer->height = WASM_HEIGHT;
    framebuffer->pitch = WASM_WIDTH * 4;
    framebuffer->format = GAME_PIXEL_FORMAT_ARGB8888;
    return true;
}

bool game_platform_present(game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl || !impl->acquired) return false;
    /* 0xAARRGGBB → 内存序 R,G,B,A (小端 uint32 = 0xAABBGGRR) */
    for(int i = 0; i < WASM_WIDTH * WASM_HEIGHT; i++) {
        uint32_t p = impl->pixels[i];
        impl->rgba[i] = (p & 0xff00ff00u) | ((p & 0xff0000u) >> 16) |
                        ((p & 0xffu) << 16);
    }
    /* ALLOW_MEMORY_GROWTH 下 HEAPU8.buffer 可能换新, 每帧重建视图才安全 */
    EM_ASM({
        var img = new ImageData(
            new Uint8ClampedArray(HEAPU8.buffer, $0, $1 * $2 * 4), $1, $2);
        Module.epassCtx.putImageData(img, 0, 0);
    }, impl->rgba, WASM_WIDTH, WASM_HEIGHT);
    impl->acquired = false;
    return true;
}

void game_platform_destroy(game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl) return;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                    false, NULL);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                  false, NULL);
    free(impl);
    platform->impl = NULL;
}

int game_platform_width(const game_platform_t *platform)
{
    return get_impl(platform) ? WASM_WIDTH : 0;
}

int game_platform_height(const game_platform_t *platform)
{
    return get_impl(platform) ? WASM_HEIGHT : 0;
}

void game_input_update(game_platform_t *platform)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl) return;
    memset(impl->pressed, 0, sizeof(impl->pressed));
    memset(impl->repeated, 0, sizeof(impl->repeated));
    uint64_t now = game_monotonic_ms();
    while(impl->event_head != impl->event_tail) {
        wasm_key_event_t *ev = &impl->events[impl->event_head];
        impl->event_head = (impl->event_head + 1) % WASM_EVENT_QUEUE;
        int key = (int)ev->key;
        if(ev->pressed) {
            if(!impl->down[key]) impl->pressed[key] = true;
            impl->down[key] = true;
            impl->next_repeat[key] = now + impl->repeat_delay;
        } else {
            impl->down[key] = false;
        }
    }
    for(int key = 0; key < GAME_KEY_COUNT; key++) {
        if(impl->down[key] && now >= impl->next_repeat[key]) {
            impl->repeated[key] = true;
            do {
                impl->next_repeat[key] += impl->repeat_interval;
            } while(now >= impl->next_repeat[key]);
        }
    }
}

void game_input_set_repeat(game_platform_t *platform, uint32_t delay_ms,
                           uint32_t interval_ms)
{
    game_platform_impl_t *impl = get_impl(platform);
    if(!impl) return;
    impl->repeat_delay = delay_ms;
    impl->repeat_interval = interval_ms ? interval_ms : 1;
}

static bool key_flag(const bool *flags, game_key_t key)
{
    return key >= 0 && key < GAME_KEY_COUNT && flags[key];
}

bool game_key_down(const game_platform_t *platform, game_key_t key)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl && key_flag(impl->down, key);
}

bool game_key_pressed(const game_platform_t *platform, game_key_t key)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl && key_flag(impl->pressed, key);
}

bool game_key_repeated(const game_platform_t *platform, game_key_t key)
{
    game_platform_impl_t *impl = get_impl(platform);
    return impl && key_flag(impl->repeated, key);
}

static game_tick_fn g_tick;
static void *g_userdata;

static void run_frame(void)
{
    if(!g_tick(g_userdata)) emscripten_cancel_main_loop();
}

void game_run(game_platform_t *platform, game_tick_fn tick, void *userdata)
{
    (void)platform;
    g_tick = tick;
    g_userdata = userdata;
    /* fps=0 → requestAnimationFrame 节拍; 不返回(模拟无限循环) */
    emscripten_set_main_loop(run_frame, 0, 1);
}

void game_platform_idle(game_platform_t *platform, uint32_t ms)
{
    (void)platform;
    (void)ms;
}
