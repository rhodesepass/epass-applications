#define _POSIX_C_SOURCE 200809L

#include "ve_check.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
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

#define PLL_VE_144M   0x91000500u /* 整数 144MHz, 测跟随性用 */

ve_check_status_t ve_check_verdict(unsigned f1_khz, unsigned f2_khz, unsigned *ratio_out)
{
    unsigned pct = (unsigned)((uint64_t)f2_khz * 100 / (f1_khz + 1));
    if(ratio_out) *ratio_out = pct;
    if(f1_khz >= 100000) return VE_CHECK_OK;
    if(f1_khz < 50000 && pct >= 85 && pct <= 115) return VE_CHECK_DEFECTIVE;
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

static void run_check(ve_check_result_t *r)
{
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

    /* 确保 VE 可访问: AHB 门控 + 复位释放 + 模块时钟 */
    static const struct { uint32_t off, bits; } gates[] = {
        { REG_AHB_GATE, 1u }, { REG_VE_RST, 1u }, { REG_VE_CLK, 0x80000000u },
    };
    for(size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        volatile uint32_t *reg = ccu + gates[i].off / 4;
        if((*reg & gates[i].bits) == 0) *reg |= gates[i].bits;
    }

    unsigned f1 = measure_khz(ve + VE_CNT_OFF / 4);

    /* 跟随性: PLL_VE 切整数 144MHz 再测, 然后恢复 */
    uint32_t pll_saved = ccu[REG_PLL_VE / 4];
    ccu[REG_PLL_VE / 4] = PLL_VE_144M;
    sleep_us(50000);
    unsigned f2 = measure_khz(ve + VE_CNT_OFF / 4);
    ccu[REG_PLL_VE / 4] = pll_saved;
    sleep_us(50000);

    munmap((void *)ccu, PAGE);
    munmap((void *)ve, PAGE);

    unsigned pct = 0;
    ve_check_status_t status = ve_check_verdict(f1, f2, &pct);
    r->f1_khz = f1;
    r->f2_khz = f2;
    r->ratio_pct = pct;
    switch(status) {
    case VE_CHECK_OK:
        snprintf(r->detail, sizeof(r->detail),
                 "PLL_VE 锁定 (~%uMHz), 跟随比 %u%%", f1 / 1000, pct);
        break;
    case VE_CHECK_DEFECTIVE:
        snprintf(r->detail, sizeof(r->detail),
                 "VE 时钟仅 %ukHz 且不随分频变化, PLL_VE 环路开路", f1);
        break;
    default:
        snprintf(r->detail, sizeof(r->detail),
                 "非典型状态 (f1=%ukHz, 跟随比 %u%%), 请人工复核", f1, pct);
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
