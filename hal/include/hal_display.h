#ifdef _WIN32
// Windows(mingw) 无 libdrm。PC/SDL 后端不接 DRM，给 drm 类型/宏占位即可——
// hal_display_t 里那几个 drm 字段在 SDL 后端根本不会被触碰。Linux PC 仍走真头。
typedef struct { int _unused; } drmVBlank;
typedef struct drmModeConnector drmModeConnector;   // 前向声明，仅作指针
typedef struct drmModeRes drmModeRes;
typedef struct drmModePlaneRes drmModePlaneRes;
#ifndef fourcc_mod_code
#define fourcc_mod_code(vendor, val) 0ULL
#endif
#else
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <semaphore.h>
#endif
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "spsc_queue.h"

#pragma once


#define DRM_FORMAT_MOD_VENDOR_ALLWINNER 0x09
#define DRM_FORMAT_MOD_ALLWINNER_TILED fourcc_mod_code(ALLWINNER, 1)

// 层号 = plane 索引 = z-order(UI 2 在最上, overlay 1 居中, video 0 最底)。
// 用 #ifndef 兜住: 迁移期间某些消费者(如 video_player)可能仍从自己的
// config.h 定义同名宏, 优先它们的, 避免重定义打架。
#ifndef HAL_DISPLAY_LAYER_VIDEO
#define HAL_DISPLAY_LAYER_VIDEO 0
#endif
#ifndef HAL_DISPLAY_LAYER_OVERLAY
#define HAL_DISPLAY_LAYER_OVERLAY 1
#endif
#ifndef HAL_DISPLAY_LAYER_UI
#define HAL_DISPLAY_LAYER_UI 2
#endif
// UI 之上的最顶层(F1C DEBE 的第 4 个 plane)。全屏遮罩/fade 用——遮罩必须
// 压在 UI 上面, OVERLAY(1) 在 UI 下面盖不住它。plane 不足 4 个的硬件上
// layer_can_fade(TOP) 返回 false, 消费者按无过渡动画退化。
#ifndef HAL_DISPLAY_LAYER_TOP
#define HAL_DISPLAY_LAYER_TOP 3
#endif

// free_queue 默认深度: 须 ≥ 该层同时在飞的 item 数(push 为阻塞语义, 不够会让
// 显示线程卡在 drain 中途死锁)。覆盖 video 最宽档(capture 16 + smooth 8 + curr
// + 储备 = 28)与游戏(2 缓冲)。有更大预算的消费者用 init_layer_ex 显式传。
#ifndef HAL_DISPLAY_FREE_QUEUE_DEPTH
#define HAL_DISPLAY_FREE_QUEUE_DEPTH 32
#endif

typedef enum {
    HAL_DISPLAY_LAYER_MODE_RGB565,
    HAL_DISPLAY_LAYER_MODE_ARGB8888,
    HAL_DISPLAY_LAYER_MODE_MB32_NV12, //allwinner specific format
    HAL_DISPLAY_LAYER_MODE_ARGB1555,
} hal_display_layer_mode_t;

typedef enum {
    // atomic：整帧翻页，fb_id 换到该层 plane 的 FB_ID 属性
    HAL_DISPLAY_ITEM_FLIP_FB,
    // atomic：CRTC_X/CRTC_Y（有符号，可为负/离屏，过渡动画依赖）
    HAL_DISPLAY_ITEM_SET_COORD,
    // atomic：plane alpha。255 = 不透明(ARGB 层回到像素 alpha)
    HAL_DISPLAY_ITEM_SET_ALPHA,
} hal_display_item_type_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t size;
    uint8_t *vaddr;
    uint32_t fb_id;
} hal_buffer_t;

typedef struct {
    hal_display_item_type_t type;
    uint32_t fb_id;         // FLIP_FB
    int16_t x, y;           // SET_COORD
    uint8_t alpha;          // SET_ALPHA
    void* userdata;
    bool on_heap;
} hal_display_queue_item_t;

typedef struct{
    bool used;
    spsc_bq_t display_queue;
    spsc_bq_t free_queue;
    hal_display_layer_mode_t mode;
    int width;
    int height;
    // 阻塞 commit 返回即旧帧离屏，在屏只押 curr 一格(试过 NONBLOCK+
    // PAGE_FLIP_EVENT 在飞翻页，要多押一个 pending，为 32MB 机型撤回)
    hal_display_queue_item_t* curr_item;
    // 惰性挂载(video 层专用)：几何先存这，显示线程翻第一帧时连同
    // CRTC_ID/SRC_*/CRTC_* 一起 commit——plane 用真实帧启用，无需黑 buffer
    bool needs_full_mount;
    int16_t geo_x, geo_y;
    int geo_src_w, geo_src_h, geo_dst_w, geo_dst_h;
} hal_display_layer_t;

// 按名字发现的 plane 属性 id（atomic commit 用）；0 = 无此属性
typedef struct {
    uint32_t fb_id, crtc_id;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
    uint32_t alpha;
    uint32_t zpos;
} hal_display_plane_prop_ids_t;

typedef struct {
  int fd;
  drmModeConnector *conn;
  drmModeRes *res;
  drmModePlaneRes *plane_res;
  uint32_t crtc_id;
  uint32_t conn_id;
  hal_display_layer_t layer[4]; // 4 layers
  uint32_t plane_ids[4];
  hal_display_plane_prop_ids_t plane_props[4];
  // 序列化 mount(同步 commit) 与显示线程的每 vsync commit
  pthread_mutex_t commit_mutex;
  drmVBlank blank;
  pthread_t display_thread;
  atomic_int thread_running;
  // 惰性启动: 显示线程只在首次 enqueue_display_item 时才起。纯同步消费者
  // (LVGL app 只调 mount_layer)永不触发, 省掉一个每 vblank 空转的线程。
  atomic_int thread_started;
} hal_display_t;



int hal_display_init(hal_display_t *hal_display);
// 停显示线程(若已惰性启动则 join)。destroy 内部也会调, 幂等; 需要"停线程→拆层
// →释放 app 缓冲→destroy"这种顺序的消费者(如 epass_game)可单独先调。
int hal_display_stop(hal_display_t *hal_display);
int hal_display_destroy(hal_display_t *hal_display);

// 当前显示模式尺寸。封装掉 conn->modes[0] 的直接访问, 让上层不再碰裸 DRM 类型
// ——非 DRM 后端(wasm/canvas)也能实现: 返回画布尺寸即可。
int hal_display_display_size(const hal_display_t *hal_display,int *width,int *height);

int hal_display_init_layer(hal_display_t *hal_display,int layer_id,int width,int height,hal_display_layer_mode_t mode);
// 同 init_layer, 但显式指定 free_queue 深度(在飞 item 多的消费者用, 如视频解码)。
int hal_display_init_layer_ex(hal_display_t *hal_display,int layer_id,int width,int height,hal_display_layer_mode_t mode,int free_queue_depth);
int hal_display_destroy_layer(hal_display_t *hal_display,int layer_id);
int hal_display_mount_layer(hal_display_t *hal_display,int layer_id,int x,int y,hal_buffer_t *buf);
// 通用:src=(0,0,src_w,src_h) 从 buf 左上角裁,dst=(x,y,dst_w,dst_h) 屏幕显示区。
// src != dst 走 DEFE 硬件缩放;src_w<buf->width 裁掉对齐 padding。裁切与缩放可组合。仅 MB32_NV12(video)层支持缩放。
int hal_display_mount_layer_rect(hal_display_t *hal_display,int layer_id,int x,int y,hal_buffer_t *buf,int src_w,int src_h,int dst_w,int dst_h);


int hal_display_allocate_buffer(hal_display_t *hal_display,int layer_id,hal_buffer_t *buf);
int hal_display_allocate_buffer_sized(hal_display_t *hal_display,int layer_id,int width,int height,hal_buffer_t *buf);
int hal_display_free_buffer(hal_display_t *hal_display,int layer_id,hal_buffer_t *buf);

// dmabuf(如 cedrus capture buffer) 导入为 NV12 + ALLWINNER_TILED 的 DRM FB。
// pitch/uv_offset 按 V4L2 G_FMT 回读值传入。gem_handle 回传 prime 句柄，
// rm_fb 时必须一并传回 close，否则 CMA 被 pin 住不归还。
int hal_display_import_dmabuf_fb(hal_display_t *hal_display,int dmabuf_fd,int width,int height,int pitch,int uv_offset,uint32_t *fb_id,uint32_t *gem_handle);
int hal_display_rm_fb(hal_display_t *hal_display,uint32_t fb_id,uint32_t gem_handle);
// 惰性挂载：只记录几何，plane 由显示线程在下一个 FLIP_FB 时携带真实帧启用。
// 调用时机须保证该层没有在飞的 FLIP(解码线程未跑)
int hal_display_set_layer_geometry(hal_display_t *hal_display,int layer_id,int x,int y,int src_w,int src_h,int dst_w,int dst_h);
// 同步 disable plane(CRTC_ID/FB_ID=0)。最底层关掉后露出 DEBE 背景色(黑)；
// 之后的 RmFB 不会碰到在屏 fb。需要 suniv quirks 内核(0014)否则可能被 atomic_check 拒
int hal_display_disable_layer_sync(hal_display_t *hal_display,int layer_id);

int hal_display_enqueue_display_item(hal_display_t *hal_display,int layer_id,hal_display_queue_item_t* item);
int hal_display_dequeue_free_item(hal_display_t *hal_display,int layer_id,hal_display_queue_item_t** out_item);
int hal_display_try_dequeue_free_item(hal_display_t *hal_display,int layer_id,hal_display_queue_item_t** out_item);

// 异步(排队, 显示线程翻帧时携带): overlay 坐标动画用。
int hal_display_set_layer_coord(hal_display_t *hal_display,int layer_id,int x,int y);

// —— 以下为 UI 过渡淡入淡出的同步基元(阻塞 commit), 映射到 wasm 后端很直接:
//    canvas 层的 CSS opacity / 显隐。仅 plane 带 alpha 属性(sun4i 上通常只 overlay)。
// 该层 plane 是否支持 alpha 混合。
bool hal_display_layer_can_fade(const hal_display_t *hal_display,int layer_id);
// 同步设该层 plane alpha(0..255)。层不支持 alpha 返回 -1。
int hal_display_set_layer_alpha(hal_display_t *hal_display,int layer_id,uint8_t alpha);
// 同步挂载并在同一次 commit 里设 fb+geometry+zpos+alpha(避免挂上瞬间以旧 alpha 闪帧)。
int hal_display_mount_layer_alpha(hal_display_t *hal_display,int layer_id,int x,int y,hal_buffer_t *buf,uint8_t alpha);
