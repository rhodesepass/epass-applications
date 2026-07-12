#define _POSIX_C_SOURCE 200809L

#include "sysinfo.h"

#include <stdio.h>
#include <string.h>

bool sysinfo_parse_memory_reg(const uint8_t *data, size_t len, unsigned *mb_out)
{
    /* F1C 是 32 位平台, #address-cells = #size-cells = 1, reg 为 (base,size) u32 对 */
    if(!data || len < 8 || len % 8 != 0) return false;
    uint64_t total = 0;
    for(size_t i = 0; i < len; i += 8) {
        const uint8_t *s = data + i + 4;
        total += ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
                 ((uint32_t)s[2] << 8) | (uint32_t)s[3];
    }
    if(total == 0) return false;
    *mb_out = (unsigned)(total / (1024 * 1024));
    return true;
}

unsigned sysinfo_round_meminfo_mb(unsigned long memtotal_kb)
{
    unsigned mb = (unsigned)(memtotal_kb / 1024);
    return (mb + 7) / 8 * 8;
}

bool sysinfo_mem_is_expected(unsigned mb)
{
    return mb == 64;
}

sysinfo_boot_t sysinfo_parse_boot_source(const char *cmdline)
{
    if(!cmdline) return SYSINFO_BOOT_UNKNOWN;
    if(strstr(cmdline, "root=/dev/mmcblk")) return SYSINFO_BOOT_SD;
    if(strstr(cmdline, "root=ubi") || strstr(cmdline, "ubi.mtd") ||
       strstr(cmdline, "root=/dev/mtdblock"))
        return SYSINFO_BOOT_NAND;
    return SYSINFO_BOOT_UNKNOWN;
}

bool sysinfo_read_model(char *buf, size_t size)
{
    FILE *fp = fopen("/proc/device-tree/model", "r");
    if(!fp) return false;
    size_t n = fread(buf, 1, size - 1, fp);
    fclose(fp);
    if(n == 0) return false;
    buf[n] = '\0';
    return true;
}

bool sysinfo_read_memory_mb(unsigned *mb_out)
{
    FILE *fp = fopen("/proc/device-tree/memory/reg", "r");
    if(fp) {
        uint8_t raw[64];
        size_t n = fread(raw, 1, sizeof(raw), fp);
        fclose(fp);
        if(sysinfo_parse_memory_reg(raw, n, mb_out)) return true;
    }
    fp = fopen("/proc/meminfo", "r");
    if(!fp) return false;
    char line[128];
    unsigned long kb = 0;
    while(fgets(line, sizeof(line), fp)) {
        if(sscanf(line, "MemTotal: %lu kB", &kb) == 1) break;
    }
    fclose(fp);
    if(kb == 0) return false;
    *mb_out = sysinfo_round_meminfo_mb(kb);
    return true;
}

sysinfo_boot_t sysinfo_read_boot_source(void)
{
    FILE *fp = fopen("/proc/cmdline", "r");
    if(!fp) return SYSINFO_BOOT_UNKNOWN;
    char line[1024];
    size_t n = fread(line, 1, sizeof(line) - 1, fp);
    fclose(fp);
    line[n] = '\0';
    return sysinfo_parse_boot_source(line);
}
