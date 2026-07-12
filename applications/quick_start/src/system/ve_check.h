#pragma once

#include <stdatomic.h>
#include <stdbool.h>

/* F1C100s/F1C200s VE 瑕疵片判定, 判据与 refresh_new_200s/tools/ve_check.sh 一致:
 * 瑕疵片 PLL_VE 收不到 24M 参考, VCO 开环贴地; 好片 297MHz 且频率严格跟随 N 分频。 */

typedef enum {
    VE_CHECK_PENDING = 0, /* 检测中 */
    VE_CHECK_OK,
    VE_CHECK_DEFECTIVE,
    VE_CHECK_UNKNOWN,
} ve_check_status_t;

typedef struct {
    _Atomic ve_check_status_t status; /* 工作线程最后写, UI 线程轮询 */
    unsigned f1_khz, f2_khz, ratio_pct;
    char detail[160];
} ve_check_result_t;

/* 纯判定 (单测覆盖): f1 基线频率, f2 切 144MHz 整数分频后的频率 */
ve_check_status_t ve_check_verdict(unsigned f1_khz, unsigned f2_khz, unsigned *ratio_out);

/* 起 detached 工作线程跑完整检测 (会短暂扰动 PLL_VE, 勿在解码中调用)。
 * result 生命周期须覆盖整个检测过程; status 变为非 PENDING 即完成。 */
bool ve_check_start(ve_check_result_t *result);
