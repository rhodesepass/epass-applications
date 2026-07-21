/*
 * system_maintenance 里真动硬件/块设备的模块的 wasm 桩。浏览器预览没有 SD 卡
 * 可格式化, 全部报"不具备条件", UI 走既有的禁用/提示路径。
 * 只由 tools/build_wasm_lvgl.sh 用 emcc 编, 替代 wasm_exclude.txt 里的文件。
 */
#ifndef __EMSCRIPTEN__
#error "system_wasm.c 只用于 emscripten 构建"
#endif

#include "sd_format.h"

#include <stdio.h>
#include <string.h>

void sd_format_init(sd_format_t *format)
{
    memset(format, 0, sizeof(*format));
    format->phase = SD_FORMAT_IDLE;
    format->mode = SD_FORMAT_MODE_UNKNOWN;
    snprintf(format->error, sizeof(format->error), "wasm 预览不支持 SD 卡操作");
}

bool sd_format_capable(const sd_format_t *format) { (void)format; return false; }

const char *sd_format_capability_reason(const sd_format_t *format)
{
    (void)format;
    return "浏览器预览环境无 SD 卡";
}

sd_format_mode_t sd_format_mode(const sd_format_t *format) { return format->mode; }

int sd_format_preflight(sd_format_t *format) { (void)format; return -1; }
int sd_format_start(sd_format_t *format) { (void)format; return -1; }
void sd_format_poll(sd_format_t *format) { (void)format; }
bool sd_format_cancel(sd_format_t *format) { (void)format; return false; }

sd_format_phase_t sd_format_phase(const sd_format_t *format) { return format->phase; }
unsigned int sd_format_progress(const sd_format_t *format) { (void)format; return 0; }
const char *sd_format_error(const sd_format_t *format) { return format->error; }
bool sd_format_finished(const sd_format_t *format) { (void)format; return false; }

int sd_format_find_tool(const char *name, const char *path_environment,
                        char *resolved, size_t resolved_size)
{
    (void)name; (void)path_environment; (void)resolved; (void)resolved_size;
    return -1; /* 浏览器里没有外部工具(tar/fdisk), 调用方按"工具缺失"报错 */
}

/* emscripten 有 sys/mount.h 头但没有实现; 挂载在浏览器里无意义 */
int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data)
{
    (void)source; (void)target; (void)fstype; (void)flags; (void)data;
    return -1;
}

int umount2(const char *target, int flags)
{
    (void)target; (void)flags;
    return -1;
}
