#ifndef SYSTEM_MAINTENANCE_BOOT_SOURCE_H
#define SYSTEM_MAINTENANCE_BOOT_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * 启动来源判据（与固件约定一致）：
 *   /proc/cmdline 含 root=/dev/mmcblk0p2  → SD 启动
 *   否则                                   → NAND 启动
 */
typedef enum {
    BOOT_SOURCE_UNKNOWN = 0,
    BOOT_SOURCE_NAND,
    BOOT_SOURCE_SD
} boot_source_t;

typedef struct {
    boot_source_t source;
    bool executable_on_sd;
    bool cwd_on_sd;
} boot_source_probe_t;

boot_source_t boot_source_from_cmdline(const char *cmdline);
bool boot_source_path_is_on_sd(const char *path);
const char *boot_source_name(boot_source_t source);

int boot_source_probe(boot_source_probe_t *result, char *error, size_t error_size);

#endif
