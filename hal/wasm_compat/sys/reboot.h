/* wasm 占位: 浏览器里没有 reboot, 调用返回失败即可 */
#pragma once
#define RB_AUTOBOOT 0x01234567
#define RB_POWER_OFF 0x4321fedc
static inline int reboot(int cmd) { (void)cmd; return -1; }
