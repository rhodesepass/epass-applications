#pragma once

#include <stdatomic.h>
#include <stdbool.h>

/* F1C100s/F1C200s VE 瑕疵片判定 (ve_check.sh v2):
 *  A 型: PLL_VE 无 24M 参考, VCO 开环 (~0.2/27MHz), LOCK 恒假。
 *  B 型: 仅分数 297MHz 有输出; 写整数模式会杀死 VCO(需断电恢复);
 *        或引擎窗口死/解码死锁。顶层寄存器与 cycle counter 仍正常。
 * 判决: 绝对频率 → 引擎窗口探针 → N 跟随(破坏性, 仅老编码 die)。 */

typedef enum {
    VE_CHECK_PENDING = 0, /* 检测中 */
    VE_CHECK_OK,
    VE_CHECK_DEFECTIVE,
    VE_CHECK_UNKNOWN,
} ve_check_status_t;

/* 引擎 select 探针结果: 老编码 sel=1; 新批次 die 带 bit3 → sel=9 */
typedef enum {
    VE_ENC_NONE = 0, /* 两种编码下窗口均死 */
    VE_ENC_OLD,      /* sel=1 可写 */
    VE_ENC_NEW,      /* sel=9 可写; 勿跑整数模式跟随 */
} ve_engine_enc_t;

typedef struct {
    _Atomic ve_check_status_t status; /* 工作线程最后写, UI 线程轮询 */
    unsigned f1_khz, f2_khz, ratio_pct;
    ve_engine_enc_t enc;
    char detail[192];
} ve_check_result_t;

/* 纯判定 (单测覆盖)。f2 仅在 enc==VE_ENC_OLD 时参与; NEW 不测跟随。 */
ve_check_status_t ve_check_verdict(unsigned f1_khz, unsigned f2_khz,
                                   ve_engine_enc_t enc, unsigned *ratio_out);

/* 起 detached 工作线程跑完整检测 (会短暂扰动 PLL_VE, 勿在解码中调用)。
 * result 生命周期须覆盖整个检测过程; status 变为非 PENDING 即完成。 */
bool ve_check_start(ve_check_result_t *result);
