#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 主循环归 HAL: 两个后端都是阻塞式 while, tick 返回 false 则 hal_run 返回
   (wasm 侧靠 ASYNCIFY 的 emscripten_sleep 让出主线程, 清理代码同样会执行)。 */
typedef bool (*hal_tick_fn)(void *userdata);
void hal_run(hal_tick_fn tick, void *userdata);

/* tick 内让出 CPU 的节流提示: epass = usleep, wasm = 空操作(rAF 自带节拍,
   浏览器里 sleep 只能忙等)。 */
void hal_idle(uint32_t ms);
