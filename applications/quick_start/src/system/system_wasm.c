/*
 * quick_start_core 带 IO 部分的 wasm 桩: 浏览器里没有 device-tree/UBI/VE 寄存器,
 * 系统信息页展示一台"标准 F1C200s"的代表性数据, 教程内容不受影响。
 * 纯解析函数(单测覆盖的那些)不在此文件——wasm 构建根本不编它们的调用方之外的部分。
 * 只由 tools/build_wasm_lvgl.sh 用 emcc 编, 替代 sysinfo.c/storage.c/ve_check.c。
 */
#ifndef __EMSCRIPTEN__
#error "system_wasm.c 只用于 emscripten 构建"
#endif

#include "sysinfo.h"
#include "storage.h"
#include "ve_check.h"

#include <stdio.h>
#include <string.h>

bool sysinfo_read_model(char *buf, size_t size)
{
    snprintf(buf, size, "epass F1C200s (wasm preview)");
    return true;
}

bool sysinfo_read_memory_mb(unsigned *mb_out)
{
    if(mb_out) *mb_out = 64;
    return true;
}

bool sysinfo_mem_is_expected(unsigned mb)
{
    return mb == 64;
}

sysinfo_boot_t sysinfo_read_boot_source(void)
{
    return SYSINFO_BOOT_NAND;
}

void storage_probe(storage_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->nand_present = true;
    snprintf(out->nand_name, sizeof(out->nand_name), "Winbond/128MB");
    out->ubi_ok = true;
    out->ubi_bad = 0;
    out->ubi_reserved = 20;
    out->sd_present = false;
}

void storage_format_size(unsigned long long bytes, char *buf, size_t size)
{
    if(bytes >= 1000000000ULL)
        snprintf(buf, size, "%.1f GB", (double)bytes / 1e9);
    else
        snprintf(buf, size, "%llu MB", bytes / 1000000ULL);
}

ve_check_status_t ve_check_verdict(unsigned f1_khz, unsigned f2_khz,
                                   ve_engine_enc_t enc, unsigned *ratio_out)
{
    (void)f1_khz; (void)f2_khz; (void)enc;
    if(ratio_out) *ratio_out = 100;
    return VE_CHECK_OK;
}

bool ve_check_start(ve_check_result_t *result)
{
    result->f1_khz = 297000;
    result->f2_khz = 297000;
    result->ratio_pct = 100;
    result->enc = VE_ENC_NEW;
    snprintf(result->detail, sizeof(result->detail), "wasm preview: skipped");
    atomic_store(&result->status, VE_CHECK_OK);
    return true;
}
