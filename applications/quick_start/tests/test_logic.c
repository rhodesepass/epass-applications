#include "storage.h"
#include "sysinfo.h"
#include "ve_check.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_memory_reg_parse(void)
{
    /* F1C200s: base 0x80000000, size 0x04000000 (64MB) */
    const uint8_t reg64[] = { 0x80, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00 };
    unsigned mb = 0;
    assert(sysinfo_parse_memory_reg(reg64, sizeof(reg64), &mb));
    assert(mb == 64);
    assert(sysinfo_mem_is_expected(mb));

    /* F1C100s: 32MB, 应触发 WARNING */
    const uint8_t reg32[] = { 0x80, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
    assert(sysinfo_parse_memory_reg(reg32, sizeof(reg32), &mb));
    assert(mb == 32);
    assert(!sysinfo_mem_is_expected(mb));

    /* 多 bank 求和: 32MB + 32MB */
    const uint8_t reg2bank[] = { 0x80, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                 0x82, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };
    assert(sysinfo_parse_memory_reg(reg2bank, sizeof(reg2bank), &mb));
    assert(mb == 64);

    /* 畸形输入 */
    assert(!sysinfo_parse_memory_reg(NULL, 8, &mb));
    assert(!sysinfo_parse_memory_reg(reg64, 0, &mb));
    assert(!sysinfo_parse_memory_reg(reg64, 7, &mb));
    const uint8_t zeros[8] = { 0 };
    assert(!sysinfo_parse_memory_reg(zeros, sizeof(zeros), &mb));
}

static void test_meminfo_round(void)
{
    /* 64MB 板 MemTotal 常见 ~55-60MB (内核保留后), 向上取整回 8MB 档 */
    assert(sysinfo_round_meminfo_mb(57340) == 56);  /* 55.99MB -> 56 */
    assert(sysinfo_round_meminfo_mb(65536) == 64);
    assert(sysinfo_round_meminfo_mb(63488) == 64);  /* 62MB -> 64 */
    assert(sysinfo_round_meminfo_mb(29000) == 32);  /* 32MB 板 ~28.3MB -> 32 */
}

static void test_ve_verdict(void)
{
    unsigned pct = 0;

    /* A 型: 绝对频率 <50MHz, 不论 enc/f2 */
    assert(ve_check_verdict(200, 200, VE_ENC_OLD, &pct) == VE_CHECK_DEFECTIVE);
    assert(ve_check_verdict(27000, 27500, VE_ENC_NONE, NULL) == VE_CHECK_DEFECTIVE);
    assert(ve_check_verdict(49999, 0, VE_ENC_NEW, NULL) == VE_CHECK_DEFECTIVE);
    /* f1=0 不除零, 仍按 A 型判 DEFECTIVE */
    assert(ve_check_verdict(0, 0, VE_ENC_OLD, &pct) == VE_CHECK_DEFECTIVE);

    /* B 型: PLL 看似正常但引擎窗口死 */
    assert(ve_check_verdict(297000, 0, VE_ENC_NONE, NULL) == VE_CHECK_DEFECTIVE);

    /* 新版 die: 窗口 sel=9 可写, 跳过整数跟随 → OK */
    assert(ve_check_verdict(297000, 0, VE_ENC_NEW, &pct) == VE_CHECK_OK);

    /* 老编码好片: 297→144 跟随 ~48% */
    assert(ve_check_verdict(297000, 144000, VE_ENC_OLD, &pct) == VE_CHECK_OK);
    assert(pct == 48);
    assert(ve_check_verdict(100000, 48000, VE_ENC_OLD, NULL) == VE_CHECK_OK);
    assert(ve_check_verdict(100000, 50000, VE_ENC_OLD, NULL) == VE_CHECK_OK); /* ~49% */
    /* 跟随比窗外 → UNKNOWN (分母 f1+1 的整数除法) */
    assert(ve_check_verdict(100000, 39000, VE_ENC_OLD, NULL) == VE_CHECK_UNKNOWN); /* 38% */
    assert(ve_check_verdict(100000, 62000, VE_ENC_OLD, NULL) == VE_CHECK_UNKNOWN); /* 61% */

    /* B 型: 整数模式杀死 VCO (f2=0) */
    assert(ve_check_verdict(297000, 0, VE_ENC_OLD, NULL) == VE_CHECK_DEFECTIVE);

    /* 非典型跟随比 → UNKNOWN (v1 只看绝对频率会误报 GOOD) */
    assert(ve_check_verdict(297000, 297000, VE_ENC_OLD, &pct) == VE_CHECK_UNKNOWN);
    assert(pct == 99);
    assert(ve_check_verdict(60000, 30000, VE_ENC_OLD, NULL) == VE_CHECK_OK); /* 50% */
}

static void test_boot_source(void)
{
    assert(sysinfo_parse_boot_source(
        "console=ttyS0,115200 root=/dev/mmcblk0p2 rootwait") == SYSINFO_BOOT_SD);
    assert(sysinfo_parse_boot_source(
        "console=ttyS0 ubi.mtd=3 root=ubi0:rootfs rootfstype=ubifs") == SYSINFO_BOOT_NAND);
    assert(sysinfo_parse_boot_source("root=/dev/mtdblock3") == SYSINFO_BOOT_NAND);
    assert(sysinfo_parse_boot_source("console=ttyS0") == SYSINFO_BOOT_UNKNOWN);
    assert(sysinfo_parse_boot_source(NULL) == SYSINFO_BOOT_UNKNOWN);
}

static void test_storage_parse(void)
{
    const char *mtd =
        "dev:    size   erasesize  name\n"
        "mtd0: 00100000 00020000 \"uboot\"\n"
        "mtd3: 07600000 0001f000 \"rootfs\"\n";
    unsigned long long size = 0;
    assert(storage_parse_mtd(mtd, 3, &size));
    assert(size == 0x07600000ULL);
    assert(storage_parse_mtd(mtd, 0, NULL)); /* size_out 可为 NULL */
    assert(!storage_parse_mtd(mtd, 1, NULL));
    /* "mtd3" 不能被 "mtd30" 之类误命中: 前缀含冒号 */
    assert(!storage_parse_mtd("mtd30: 00010000 00001000 \"x\"\n", 3, NULL));
    assert(!storage_parse_mtd(NULL, 3, NULL));

    unsigned long long bytes = 0;
    assert(storage_parse_block_sectors("31116288\n", &bytes));
    assert(bytes == 31116288ULL * 512);
    assert(!storage_parse_block_sectors("0\n", &bytes));
    assert(!storage_parse_block_sectors("", &bytes));
    assert(!storage_parse_block_sectors(NULL, &bytes));

    char buf[32];
    storage_format_size(31116288ULL * 512, buf, sizeof(buf)); /* ~15.9 GB */
    assert(strcmp(buf, "15.9 GB") == 0);
    storage_format_size(512ULL * 1000 * 1000, buf, sizeof(buf));
    assert(strcmp(buf, "512 MB") == 0);
    storage_format_size(2ULL * 1000 * 1000 * 1000, buf, sizeof(buf));
    assert(strcmp(buf, "2.0 GB") == 0);
}

int main(void)
{
    test_memory_reg_parse();
    test_meminfo_round();
    test_ve_verdict();
    test_boot_source();
    test_storage_parse();
    printf("quick_start logic tests passed\n");
    return 0;
}
