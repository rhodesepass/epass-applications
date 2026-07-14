#define _POSIX_C_SOURCE 200809L

#include "ve_check.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CCU_BASE      0x01c20000u
#define VE_BASE       0x01c0e000u
#define PAGE          4096u

/* CCU 页内偏移 */
#define REG_PLL_VE    0x018 /* PLL_VE 控制 */
#define REG_AHB_GATE  0x064 /* bit0: VE AHB 门控 */
#define REG_VE_CLK    0x13c /* bit31: VE 模块时钟 */
#define REG_VE_RST    0x2c4 /* bit0: VE 复位释放 */

#define VE_CNT_OFF    0x008 /* VE cycle counter */
#define VE_H264_IE    0x220 /* H264 中断使能 (掩码 0x7) */

#define PLL_VE_144M   0x91000500u /* 整数 144MHz, 测跟随性用 (仅老编码) */

ve_check_status_t ve_check_verdict(unsigned f1_khz, unsigned f2_khz,
                                   ve_engine_enc_t enc, unsigned *ratio_out)
{
    unsigned pct = (unsigned)((uint64_t)f2_khz * 100 / (f1_khz + 1));
    if(ratio_out) *ratio_out = pct;

    /* 1. 绝对频率 → A 型 */
    if(f1_khz < 50000) return VE_CHECK_DEFECTIVE;

    /* 2. 引擎窗口 → B 型(窗口死) / 新版 die(跳过跟随) */
    if(enc == VE_ENC_NONE) return VE_CHECK_DEFECTIVE;
    if(enc == VE_ENC_NEW) return VE_CHECK_OK;

    /* 3. N 跟随 (仅老编码) */
    if(f2_khz == 0) return VE_CHECK_DEFECTIVE;
    if(pct >= 40 && pct <= 60) return VE_CHECK_OK;
    return VE_CHECK_UNKNOWN;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void sleep_us(long us)
{
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

/* 读 /proc/interrupts 中 video-codec 计数; 失败返回 -1 */
static long ve_irq_count(void)
{
    FILE *f = fopen("/proc/interrupts", "r");
    if(!f) return -1;
    char line[256];
    long n = -1;
    while(fgets(line, sizeof(line), f)) {
        if(!strstr(line, "video-codec")) continue;
        /* " 42:  12345  ...  video-codec" — 取第一个计数字段 */
        const char *p = strchr(line, ':');
        if(p) {
            char *end = NULL;
            long v = strtol(p + 1, &end, 10);
            if(end != p + 1) n = v;
        }
        break;
    }
    fclose(f);
    return n;
}

/* 400ms 窗口测 VE 时钟, 返回 kHz; 计数器 31 位回绕由掩码处理 */
static unsigned measure_khz(volatile uint32_t *ve_cnt)
{
    *ve_cnt = 0x80000000u;
    uint32_t c0 = *ve_cnt;
    uint64_t t0 = now_ms();
    sleep_us(400000);
    uint32_t c1 = *ve_cnt;
    uint64_t ms = now_ms() - t0;
    if(ms == 0) ms = 400;
    return (unsigned)(((c1 - c0) & 0x7FFFFFFFu) / ms);
}

/* H264 中断使能窗口探针: select 写入后读回 0x7 则窗口活 */
static bool probe_h264(volatile uint32_t *ve, uint32_t sel)
{
    ve[0] = sel;
    volatile uint32_t *ie = ve + VE_H264_IE / 4;
    *ie = 0x7u;
    uint32_t v = *ie;
    *ie = 0;
    return v == 7u;
}

static void run_check(ve_check_result_t *r)
{
    /* 0. VE 必须空闲 — 解码中扰动会毁掉当前帧甚至杀 PLL */
    long i0 = ve_irq_count();
    if(i0 >= 0) {
        sleep_us(1000000);
        long i1 = ve_irq_count();
        if(i1 >= 0 && i1 != i0) {
            snprintf(r->detail, sizeof(r->detail),
                     "ABORT: VE 正在解码 (irq %ld→%ld), 先停掉解码", i0, i1);
            atomic_store(&r->status, VE_CHECK_UNKNOWN);
            return;
        }
    }

    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if(fd < 0) {
        snprintf(r->detail, sizeof(r->detail), "无法打开 /dev/mem");
        atomic_store(&r->status, VE_CHECK_UNKNOWN);
        return;
    }
    volatile uint32_t *ccu = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CCU_BASE);
    volatile uint32_t *ve = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, VE_BASE);
    close(fd);
    if(ccu == MAP_FAILED || ve == MAP_FAILED) {
        if(ccu != MAP_FAILED) munmap((void *)ccu, PAGE);
        if(ve != MAP_FAILED) munmap((void *)ve, PAGE);
        snprintf(r->detail, sizeof(r->detail), "寄存器映射失败");
        atomic_store(&r->status, VE_CHECK_UNKNOWN);
        return;
    }

    /* 1. 确保 VE 可访问: AHB 门控 + 复位释放 + 模块时钟 */
    static const struct { uint32_t off, bits; } gates[] = {
        { REG_AHB_GATE, 1u }, { REG_VE_RST, 1u }, { REG_VE_CLK, 0x80000000u },
    };
    for(size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        volatile uint32_t *reg = ccu + gates[i].off / 4;
        if((*reg & gates[i].bits) == 0) *reg |= gates[i].bits;
    }

    /* 2. 基线频率 (297MHz 分数模式原样) */
    unsigned f1 = measure_khz(ve + VE_CNT_OFF / 4);
    r->f1_khz = f1;
    r->f2_khz = 0;
    r->ratio_pct = 0;
    r->enc = VE_ENC_NONE;

    if(f1 < 50000) {
        munmap((void *)ccu, PAGE);
        munmap((void *)ve, PAGE);
        snprintf(r->detail, sizeof(r->detail),
                 "DEFECTIVE(A): VE 仅 %ukHz — PLL_VE 参考缺失, VCO 自由振荡", f1);
        atomic_store(&r->status, VE_CHECK_DEFECTIVE);
        return;
    }

    /* 3. 引擎窗口探针 (老 sel=1 / 新 sel=9) */
    ve_engine_enc_t enc = VE_ENC_NONE;
    if(probe_h264(ve, 1)) enc = VE_ENC_OLD;
    else if(probe_h264(ve, 9)) enc = VE_ENC_NEW;
    ve[0] = 7; /* 引擎禁用态, 不留脏 select */
    r->enc = enc;

    if(enc == VE_ENC_NONE) {
        munmap((void *)ccu, PAGE);
        munmap((void *)ve, PAGE);
        snprintf(r->detail, sizeof(r->detail),
                 "DEFECTIVE(B): PLL ~%uMHz 但 H264 窗口两种编码下都死", f1 / 1000);
        atomic_store(&r->status, VE_CHECK_DEFECTIVE);
        return;
    }

    if(enc == VE_ENC_NEW) {
        munmap((void *)ccu, PAGE);
        munmap((void *)ve, PAGE);
        /* 新版 die: VCO 有振荡下限, N=5 会停振且断电前不可恢复 — 不跑跟随 */
        snprintf(r->detail, sizeof(r->detail),
                 "NEW-REV: 新F1C200s(CB批次), PLL ~%uMHz, 窗口可写; "
                 , f1 / 1000);
        atomic_store(&r->status, VE_CHECK_OK);
        return;
    }

    /* 4. N 跟随 — 破坏性: 仅老编码; B 型写整数会杀死 PLL_VE */
    uint32_t pll_saved = ccu[REG_PLL_VE / 4];
    ccu[REG_PLL_VE / 4] = PLL_VE_144M;
    sleep_us(50000);
    unsigned f2 = measure_khz(ve + VE_CNT_OFF / 4);
    ccu[REG_PLL_VE / 4] = pll_saved;
    sleep_us(50000);

    munmap((void *)ccu, PAGE);
    munmap((void *)ve, PAGE);

    unsigned pct = 0;
    ve_check_status_t status = ve_check_verdict(f1, f2, VE_ENC_OLD, &pct);
    r->f2_khz = f2;
    r->ratio_pct = pct;

    switch(status) {
    case VE_CHECK_OK:
        snprintf(r->detail, sizeof(r->detail),
                 "GOOD: PLL_VE ~%uMHz, 整数跟随 %u%%, 引擎窗口可写",
                 f1 / 1000, pct);
        break;
    case VE_CHECK_DEFECTIVE:
        if(f2 == 0) {
            snprintf(r->detail, sizeof(r->detail),
                     "DEFECTIVE(B): 整数模式无输出, VCO 已死 — 需断电恢复 297 模式");
        } else {
            snprintf(r->detail, sizeof(r->detail),
                     "DEFECTIVE: f1=%ukHz f2=%ukHz 跟随比 %u%%", f1, f2, pct);
        }
        break;
    default:
        snprintf(r->detail, sizeof(r->detail),
                 "UNKNOWN: 非典型 (f1=%ukHz f2=%ukHz pct=%u%%), 人工复核",
                 f1, f2, pct);
        break;
    }
    atomic_store(&r->status, status);
}

static void *check_thread(void *arg)
{
    run_check(arg);
    return NULL;
}

bool ve_check_start(ve_check_result_t *result)
{
    memset(result, 0, sizeof(*result));
    atomic_store(&result->status, VE_CHECK_PENDING);
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, check_thread, result);
    pthread_attr_destroy(&attr);
    if(rc != 0) {
        snprintf(result->detail, sizeof(result->detail), "检测线程启动失败");
        atomic_store(&result->status, VE_CHECK_UNKNOWN);
        return false;
    }
    return true;
}
