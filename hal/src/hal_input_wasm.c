/*
 * hal_input 的 wasm/浏览器后端: window 键盘事件进环形队列, 别名表与 epass
 * 后端(evdev)一致。浏览器 autorepeat 原样透传为 ev.repeat——与内核 evdev 的
 * value==2 语义相同, 由消费者自行取舍。
 * 只由 tools/build_wasm_lvgl.sh 用 emcc 编, 不进 CMake。
 */
#ifndef __EMSCRIPTEN__
#error "hal_input_wasm.c 只用于 emscripten 构建"
#endif

#include "hal_input.h"

#include <emscripten/html5.h>
#include <string.h>

#define WASM_EVENT_QUEUE 32

/* 单显示单输入, 静态实例即可; 键盘回调与 tick 同在浏览器主线程, 无并发 */
static hal_input_event_t queue[WASM_EVENT_QUEUE];
static int head, tail;

static int map_code(const char *code)
{
    if(!strcmp(code, "Digit1") || !strcmp(code, "ArrowUp") ||
       !strcmp(code, "ArrowLeft")) return HAL_KEY_1;
    if(!strcmp(code, "Digit2") || !strcmp(code, "ArrowDown") ||
       !strcmp(code, "ArrowRight")) return HAL_KEY_2;
    if(!strcmp(code, "Digit3") || !strcmp(code, "Enter")) return HAL_KEY_3;
    if(!strcmp(code, "Digit4") || !strcmp(code, "Escape")) return HAL_KEY_4;
    return -1;
}

static EM_BOOL key_callback(int type, const EmscriptenKeyboardEvent *ev,
                            void *userdata)
{
    (void)userdata;
    int key = map_code(ev->code);
    if(key < 0) return EM_FALSE; /* 不认识的键还给浏览器 */
    int next = (tail + 1) % WASM_EVENT_QUEUE;
    if(next == head) return EM_TRUE; /* 队满丢弃 */
    queue[tail].key = (hal_key_t)key;
    queue[tail].pressed = type == EMSCRIPTEN_EVENT_KEYDOWN;
    queue[tail].repeat = type == EMSCRIPTEN_EVENT_KEYDOWN && ev->repeat;
    tail = next;
    return EM_TRUE; /* 吃掉, 防页面滚动 */
}

int hal_input_init(hal_input_t *in)
{
    if(!in) return 0;
    memset(in, 0, sizeof(*in));
    head = tail = 0;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                    false, key_callback);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                  false, key_callback);
    in->fd_count = 1; /* 假装一个设备, 让 "<=0 即失败" 的调用方通过 */
    return 1;
}

void hal_input_destroy(hal_input_t *in)
{
    if(!in) return;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                    false, NULL);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL,
                                  false, NULL);
    in->fd_count = 0;
}

bool hal_input_next_event(hal_input_t *in, hal_input_event_t *ev)
{
    if(!in || !ev || head == tail) return false;
    *ev = queue[head];
    head = (head + 1) % WASM_EVENT_QUEUE;
    return true;
}
