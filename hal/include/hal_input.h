#pragma once

#include <stdbool.h>

/*
 * 4 键导航输入。物理键 KEY_1..4, 并折叠常用键盘别名方便接 USB 键盘调试:
 *   KEY_1 | KEY_UP   | KEY_LEFT  -> HAL_KEY_1 (上/上一项)
 *   KEY_2 | KEY_DOWN | KEY_RIGHT -> HAL_KEY_2 (下/下一项)
 *   KEY_3 | KEY_ENTER            -> HAL_KEY_3 (确认)
 *   KEY_4 | KEY_ESC  | KEY_BACK  -> HAL_KEY_4 (返回)
 * 键到动作(上一页/方向上...)的映射是应用语义, 留在 app 侧。
 */

#define HAL_INPUT_MAX_FDS 16

typedef enum {
    HAL_KEY_1 = 0,
    HAL_KEY_2,
    HAL_KEY_3,
    HAL_KEY_4,
    HAL_KEY_COUNT
} hal_key_t;

typedef struct {
    hal_key_t key;
    bool pressed;   // true = 按下(含 autorepeat), false = 松开
    bool repeat;    // 内核 autorepeat 重报 (evdev value==2); 松开事件恒为 false
} hal_input_event_t;

typedef struct {
    int fds[HAL_INPUT_MAX_FDS];
    int fd_count;
} hal_input_t;

/* 扫描 /dev/input/event*, 打开所有带导航键的设备(非阻塞)。返回设备数, 0 = 没找到。 */
int hal_input_init(hal_input_t *in);
void hal_input_destroy(hal_input_t *in);

/* 非阻塞取下一个已翻译的键事件; 没有则返回 false。非导航键被吞掉。 */
bool hal_input_next_event(hal_input_t *in, hal_input_event_t *ev);
