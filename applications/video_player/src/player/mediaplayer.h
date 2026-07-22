/*
 * Fork 自 drm_app_neo 的 mediaplayer（cedrus V4L2 request API 硬解 →
 * dmabuf FB → hal_display atomic 翻页），为播放器加了：
 *  - 暂停（pacer 层，唯一定速点也是唯一暂停点，pacer 恒开）
 *  - 关键帧级 seek（异步请求，解码线程在循环顶部执行）
 *  - cedrus-rotate 硬件旋转（0/90/180/270，运行时可切）；倒装屏
 *    (srgn,scanout-yflip)自动给视频层补 SDROT VFLIP（boe-flip-180.md）
 *  - 播放进度/时长查询（UI 拉取）
 * EOS 行为与上游一致：回绕循环播放。
 */
#pragma once

#include "config.h"

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

#include "hal_display.h"

#define MEDIAPLAYER_DECODER_ERROR  (1 << 1)
#define MEDIAPLAYER_DECODER_EXIT   (1 << 4)

typedef struct MultiThreadCtx {
    pthread_rwlock_t rwlock;
    int state;
    int requested_stop;
} MultiThreadCtx;

typedef struct {
    void                *priv;
    bool                 session_open;

    pthread_t            decode_thread;
    MultiThreadCtx       thread;

    char                 input_path[256];
    atomic_int           running;
    uint32_t             frame_duration_us;

    /* 未从 free_queue 回流的帧 item 数；stop/seek/切旋转时据此等待离屏 */
    int                  items_in_flight;
    /* 会话代号，进 item userdata 高位；旋转切换会重建 buffer 池，旧代
     * item 迟到回流时只计数不碰新账本 */
    uint32_t             session_gen;

    int                  frame_width;
    int                  frame_height;
    int                  display_width;
    int                  display_height;

    int                  screen_width;
    int                  screen_height;

    /* UI 线程 → 解码/pacer 线程的控制邮箱；解码线程在循环顶部消化 */
    atomic_int           paused;
    atomic_int           step_one;       /* 暂停中 seek 后放行一帧 */
    atomic_int           seek_pending;
    atomic_uint          seek_target_ms;
    atomic_int           rot_request;    /* 期望角度 */
    atomic_int           rot_angle;      /* 当前生效角度（UI 只读） */

    atomic_uint          pos_ms;         /* pacer 上屏时刷新 */
    atomic_uint          duration_ms;

    /* seek/旋转切换期间解码线程停 pacer 的握手 */
    atomic_int           pacer_hold;
    atomic_int           pacer_parked;

    hal_display_t       *hal_display;
} mediaplayer_t;

typedef enum {
    MP_STATUS_PLAYING,
    MP_STATUS_PAUSED,
    MP_STATUS_STOPPED,
    MP_STATUS_ERROR,
} mp_status_t;

int mediaplayer_init(mediaplayer_t *mp, hal_display_t *hal_display,
                     int screen_width, int screen_height);
int mediaplayer_destroy(mediaplayer_t *mp);

/* 开播（非阻塞，循环播放）。横屏素材自动带 VP_ROT_CW_ANGLE 旋转。 */
int mediaplayer_play_video(mediaplayer_t *mp, const char *file);
int mediaplayer_stop(mediaplayer_t *mp);

mp_status_t mediaplayer_get_status(mediaplayer_t *mp);

void mediaplayer_set_paused(mediaplayer_t *mp, bool paused);
bool mediaplayer_get_paused(mediaplayer_t *mp);

/* 关键帧级 seek，异步；实际落点是 target 之前最近的 IDR */
int mediaplayer_seek_ms(mediaplayer_t *mp, uint32_t target_ms);

/* 异步切旋转角度（0/90/180/270）；切换瞬间视频层黑一拍 */
int mediaplayer_set_rotation(mediaplayer_t *mp, int angle);
int mediaplayer_get_rotation(mediaplayer_t *mp);

uint32_t mediaplayer_get_position_ms(mediaplayer_t *mp);
uint32_t mediaplayer_get_duration_ms(mediaplayer_t *mp);
