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

    /* 好片: 297MHz 基线, 切 144MHz 后跟随 (~48%) */
    assert(ve_check_verdict(297000, 144000, &pct) == VE_CHECK_OK);
    assert(pct == 48);
    /* 好片边界: 恰好 100MHz */
    assert(ve_check_verdict(100000, 48000, NULL) == VE_CHECK_OK);

    /* 瑕疵片: VCO 开环贴地, 频率不随 N 变化 (跟随比 ~100%) */
    assert(ve_check_verdict(200, 200, &pct) == VE_CHECK_DEFECTIVE);
    assert(ve_check_verdict(27000, 27500, NULL) == VE_CHECK_DEFECTIVE);
    /* 跟随比边界 85 / 115 (整数除法, 分母 f1+1) */
    assert(ve_check_verdict(9999, 8500, NULL) == VE_CHECK_DEFECTIVE);    /* =85% */
    assert(ve_check_verdict(9999, 11500, NULL) == VE_CHECK_DEFECTIVE);   /* =115% */
    assert(ve_check_verdict(10000, 8400, NULL) == VE_CHECK_UNKNOWN);     /* 83% */
    assert(ve_check_verdict(10000, 11700, NULL) == VE_CHECK_UNKNOWN);    /* 116% */

    /* 灰区: 50-100MHz 一律 UNKNOWN */
    assert(ve_check_verdict(60000, 30000, NULL) == VE_CHECK_UNKNOWN);
    assert(ve_check_verdict(99999, 48000, NULL) == VE_CHECK_UNKNOWN);
    /* f1=0 不除零, 比值 0% 不在跟随窗口 → UNKNOWN */
    assert(ve_check_verdict(0, 0, &pct) == VE_CHECK_UNKNOWN);
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

int main(void)
{
    test_memory_reg_parse();
    test_meminfo_round();
    test_ve_verdict();
    test_boot_source();
    printf("quick_start logic tests passed\n");
    return 0;
}
