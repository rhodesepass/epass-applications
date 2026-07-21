/*
 * hal_run 的 wasm 后端。构建须开 -sASYNCIFY: emscripten_sleep 会真正挂起
 * wasm 并让出浏览器主线程, 于是 hal_run 可以用与 epass 完全相同的阻塞式
 * while——每次 hal_idle 浏览器都能重绘, 平台层的阻塞式过渡(逐步改 alpha)
 * 也能逐帧显示。不能用 emscripten_set_main_loop: asyncify 挂起不允许发生
 * 在 rAF 回调内部(实测整页冻死)。
 * 只由 tools/build_wasm_lvgl.sh 用 emcc 编, 不进 CMake。
 */
#ifndef __EMSCRIPTEN__
#error "hal_run_wasm.c 只用于 emscripten 构建"
#endif

#include "hal_run.h"

#include <emscripten.h>

void hal_run(hal_tick_fn tick, void *userdata)
{
    /* tick 内部通常已 hal_idle; sleep(0) 兜底保证每圈至少让出一次 */
    while(tick(userdata)) emscripten_sleep(0);
}

void hal_idle(uint32_t ms)
{
    emscripten_sleep(ms ? ms : 1);
}
