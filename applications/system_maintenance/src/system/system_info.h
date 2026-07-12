#ifndef SYSTEM_MAINTENANCE_SYSTEM_INFO_H
#define SYSTEM_MAINTENANCE_SYSTEM_INFO_H

#include <stddef.h>

#define SYSTEM_INFO_FIELD_LENGTH 96U

/*
 * 来自 /etc/os-release（buildroot 生成）：
 *   NAME=ArkEPass
 *   VERSION=a2.7.0-17-ga276145   → 系统（固件）版本
 *   VERSION_ID=2020.02.7         → buildroot 版本
 */
typedef struct {
    char name[SYSTEM_INFO_FIELD_LENGTH];
    char version[SYSTEM_INFO_FIELD_LENGTH];
    char buildroot[SYSTEM_INFO_FIELD_LENGTH];
} system_info_t;

void system_info_init(system_info_t *info);
int system_info_parse_os_release(const char *text, system_info_t *info);
int system_info_load(system_info_t *info);

#endif
