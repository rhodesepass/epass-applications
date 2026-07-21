#pragma once

// 移植自 drm_app_neo 的 driver/vdec/player 代码经 #include "config.h" 取配置；
// 本文件只保留播放器用得到的子集，数值口径与上游一致（预算注释见上游 config.h）。

// ========== Display Layer ==========

// ========== 解码 buffer 预算（与 drm_app_neo 相同）==========
#define VDEC_OUTPUT_BUF_SIZE (512 * 1024)
#define VDEC_OUTPUT_BUF_COUNT 2
#define VDEC_REORDER_DEPTH 2
#define VDEC_CAPTURE_BUF_MIN 5
#define VDEC_CAPTURE_BUF_MAX_LARGE 8
#define VDEC_CAPTURE_BUF_MAX_SMALL 16
#define VDEC_CAPTURE_LARGE_AREA (600 * 1000)

// 平滑 ring：本播放器恒开 pacer（pause/seek 的唯一控制点），小内存机也押 1 格
#define MP_SMOOTH_BUFS_SMALL_MEM 1
#define MP_SMOOTH_BUFS_LARGE_MEM 8
#define MP_MEM_LARGE_THRESHOLD_KB (40 * 1024)
#define MP_SMOOTH_BUFS_MAX 8

#define SYSINFO_MEMINFO_PATH "/proc/meminfo"

// ========== cedrus-rotate ==========
// 旋转输出池：解码线程手里 1 + 显示链路押的 1-2 + 少量 ring 储备。比
// smooth ring 的理论需求小，靠反压限流；再大只是多吃 CMA（720p 旋转时
// 每格 ~1.4MB）。
#define VP_ROT_BUFS 4
// 硬件 90° 顺逆未验证（cedrus-rotate-usage.md 风险 2）：横屏素材立起来
// 该转的角度。真机验证方向反了就改成 270。
#define VP_ROT_CW_ANGLE 90

// ========== 播放条 UI ==========
#define VP_BAR_WIDTH_REF 360   // 360x640 基准屏下的条尺寸；其他屏按宽度等比放大
#define VP_BAR_HEIGHT_REF 132
#define VP_FONT_PX 28          // 主字号(时间/状态)，FreeType
#define VP_FONT_HELP_PX 16     // 使用说明行
#define VP_KEY_LONGPRESS_MS 800
#define VP_BAR_AUTOHIDE_MS 3000
#define VP_SCRUB_COMMIT_MS 700    // 拖动停手后预览 seek
#define VP_SCRUB_EXIT_MS 2500     // 预览后无操作自动确认退出拖动
#define VP_SCRUB_STEP_MS 5000
