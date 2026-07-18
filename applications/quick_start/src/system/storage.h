#pragma once

#include <stdbool.h>
#include <stddef.h>

/* ---- 纯解析, 无 IO (单测覆盖) ---- */

/* /proc/mtd 里是否有 mtd<index> 分区; 命中则填其大小 (字节, size 列为 16 进制) */
bool storage_parse_mtd(const char *proc_mtd, int index,
                       unsigned long long *size_out);

/* /sys/block/<dev>/size 内容 (512B 扇区数, 10 进制文本) → 字节 */
bool storage_parse_block_sectors(const char *text, unsigned long long *bytes_out);

/* 字节 → 人类可读, 按 SD 卡标称的 10 进制单位 ("14.8 GB" / "512 MB") */
void storage_format_size(unsigned long long bytes, char *buf, size_t size);

/* dmesg 文本 → "Winbond/128MB" (厂商来自 "X SPI NAND was found", 容量来自 "N MiB,") */
bool storage_parse_nand_dmesg(const char *dmesg, char *out, size_t out_size);

/* ---- 带 IO 的封装 ---- */

typedef struct {
    bool nand_present;              /* mtd3 存在 (NAND 已探到并分区) */
    char nand_name[40];             /* dmesg 提取的 "厂商/容量", 空则未解析到 */
    bool ubi_ok;                   /* 成功读到 UBI 统计 */
    int  ubi_bad;                  /* 已知坏块 (bad_peb_count) */
    int  ubi_reserved;             /* 还可容纳的坏块 (reserved_for_bad) */
    bool sd_present;
    unsigned long long sd_bytes;   /* SD 设备总大小 */
} storage_info_t;

/* 探测当前存储. NAND 启动时 ubi0 已挂着直接读 sysfs; SD 启动 UBI 没挂时
 * 临时 UBI_IOCATT 挂上 mtd3 读完再 UBI_IOCDET 卸下 (读完即走, 不留痕)。 */
void storage_probe(storage_info_t *out);
