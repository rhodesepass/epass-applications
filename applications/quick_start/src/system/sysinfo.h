#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- 纯解析, 无 IO (单测覆盖) ---- */

/* device-tree memory/reg 原始字节: (base,size) 的 big-endian u32 对, 各对 size 求和 → MB */
bool sysinfo_parse_memory_reg(const uint8_t *data, size_t len, unsigned *mb_out);

/* MemTotal(kB) 向上取整到 8MB 档 (内核保留导致 meminfo 恒小于物理值) */
unsigned sysinfo_round_meminfo_mb(unsigned long memtotal_kb);

/* F1C200s 预期 64MB */
bool sysinfo_mem_is_expected(unsigned mb);

typedef enum {
    SYSINFO_BOOT_NAND,
    SYSINFO_BOOT_SD,
    SYSINFO_BOOT_UNKNOWN,
} sysinfo_boot_t;

/* 从内核 cmdline 判断启动介质 (SD 启动 root 在 mmcblk, NAND 在 ubi/mtd) */
sysinfo_boot_t sysinfo_parse_boot_source(const char *cmdline);

/* ---- 带 IO 的封装 ---- */

/* /proc/device-tree/model, NUL 结尾 */
bool sysinfo_read_model(char *buf, size_t size);

/* 物理内存 MB: device-tree memory/reg 优先, 回退 /proc/meminfo MemTotal */
bool sysinfo_read_memory_mb(unsigned *mb_out);

/* /proc/cmdline → 启动介质 */
sysinfo_boot_t sysinfo_read_boot_source(void);
