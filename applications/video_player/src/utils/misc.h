#pragma once

#include <stdint.h>

// 从 drm_app_neo utils/misc 摘出的最小子集：完整版牵着 cJSON/SD 配置一串
// 启动器专属依赖，播放器只需要两个时钟。
uint64_t get_now_us(void);
uint64_t get_mono_us(void);  // 单调时钟（不受墙钟跳变影响），用于计时/动画/超时
