/*
 * Fork 自 drm_app_neo src/render/mediaplayer.c，播放管线骨架不变：
 *
 * 解码线程全速解码(demux 是内存 sample 表、request 是同步等待)，帧进
 * smooth_q；pacer 线程按档期取出上屏。VE spike 只堵解码侧。
 * DPB 只由解码线程碰，pacer 只搬 item，无需加锁。
 *
 * 本 fork 的差异：
 *  - pacer 恒开(ring >= 1)，是唯一定速点：暂停、单步都收敛在 pacer；
 *    解码线程 push 改 try_push 轮询——暂停时 ring 满不能把解码线程堵死
 *    在 push 上，否则 seek/旋转/stop 请求永远轮不到执行。
 *  - seek 与旋转切换由解码线程在循环顶部执行(demux/parser/DPB/rot 会话
 *    只有它碰)，执行前先停住 pacer(握手)、排空 smooth_q、等显示端回流。
 *  - 旋转开启时帧先过 cedrus-rotate(同步,~1ms)：显示端押的是 rot 池
 *    buffer，解码 slot 在旋转完成后立即可复用(只 mark_displayed，不
 *    set_on_screen)，item userdata 里多一位 kind 区分两种 buffer。
 */

#include "mediaplayer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "hal_display.h"
#include "utils/log.h"
#include "config.h"
#include "utils/misc.h"
#include "utils/spsc_queue.h"

#include "vdec/nalu.h"
#include "vdec/mp4_demux.h"
#include "vdec/h264_parser.h"
#include "vdec/h264_dpb.h"
#include "vdec/vdec_v4l2.h"
#include "vdec/rotate_v4l2.h"

#define mp_get_now_us get_now_us

#define MP_UD_KIND_SLOT 0
#define MP_UD_KIND_ROT  1

typedef struct {
    struct mp4_demux   demux;
    struct h264_parser parser;
    struct h264_dpb    dpb;
    struct vdec_ctx    vdec;
    uint32_t           fb_ids[VDEC_MAX_CAP_BUFS];
    uint32_t           gem_handles[VDEC_MAX_CAP_BUFS];

    spsc_bq_t          smooth_q;
    bool               smooth_q_ready;
    pthread_t          pacer_thread;
    bool               pacer_started;
    atomic_int         pacer_running;
    unsigned int       smooth_bufs;

    /* cedrus-rotate 会话 + 输出池(显示端押的就是这些 buffer) */
    struct rot_ctx     rot;
    bool               rot_fd_open;
    bool               rot_active;
    bool               rot_resetup;   /* rot_run 报会话故障，循环顶部重建 */
    uint32_t           rot_fb_ids[ROT_MAX_CAP_BUFS];
    uint32_t           rot_gems[ROT_MAX_CAP_BUFS];
    int                rot_free[ROT_MAX_CAP_BUFS];
    int                rot_free_n;

    /* 显示端(队列/在屏)手里押着哪些 buffer；只有解码线程读写。
     * seek 后按它把在屏 slot 重新 set_on_screen、重建 rot 空闲表 */
    bool               hold_slot[VDEC_MAX_CAP_BUFS];
    bool               hold_rot[ROT_MAX_CAP_BUFS];

    /* item 的显示时刻旁路表，pacer 上屏时查(经队列传递有 happens-before) */
    uint32_t           pts_slot[VDEC_MAX_CAP_BUFS];
    uint32_t           pts_rot[ROT_MAX_CAP_BUFS];

    /* seek 重建 DPB 用的会话参数 */
    int                dpb_capacity;
    int                dpb_max_ref;
    int                dpb_max_frame_num;
    int                dpb_reorder;

    /* 码流游标(原版是解码线程局部变量，seek 要改它所以进 priv) */
    unsigned int       sample_idx;
    bool               pending_flush;
    bool               wrap_pending;   /* 本次 flush 是 EOS 回绕，排空后归零时间轴 */

    uint64_t           seek_base_ms;
    uint32_t           display_seq;

    int                cur_angle;
} mp_dev_priv_t;

static int mp_set_display_size(mediaplayer_t *mp, const struct h264_sps *sps)
{
    unsigned int crop_unit_x = 1;
    unsigned int crop_unit_y = 1;
    unsigned int crop_x, crop_y;

    mp->display_width = mp->frame_width;
    mp->display_height = mp->frame_height;
    if (!sps->frame_cropping_flag)
        return 0;

    /* H.264 7.4.2.1.1 crop units；隔行流已在调用前拒绝。 */
    if (!sps->separate_colour_plane_flag) {
        if (sps->chroma_format_idc == 1 || sps->chroma_format_idc == 2)
            crop_unit_x = 2;
        if (sps->chroma_format_idc == 1)
            crop_unit_y = 2;
    }

    crop_x = (sps->frame_crop_left_offset + sps->frame_crop_right_offset) *
             crop_unit_x;
    crop_y = (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset) *
             crop_unit_y;
    if (crop_x >= (unsigned int)mp->frame_width ||
        crop_y >= (unsigned int)mp->frame_height) {
        log_error("invalid SPS crop %ux%u for coded size %dx%d",
                  crop_x, crop_y, mp->frame_width, mp->frame_height);
        return -1;
    }

    mp->display_width -= (int)crop_x;
    mp->display_height -= (int)crop_y;
    return 0;
}

static inline unsigned int mp_slow_threshold_us(const mediaplayer_t *mp)
{
    return mp->frame_duration_us + mp->frame_duration_us / 2;
}

static unsigned int mp_smooth_bufs(void)
{
    static int cached = -1;
    unsigned long total_kb = 0;
    char line[128];
    FILE *f;

    if (cached >= 0)
        return (unsigned int)cached;

    f = fopen(SYSINFO_MEMINFO_PATH, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %lu kB", &total_kb) == 1)
                break;
        }
        fclose(f);
    }
    if (!total_kb) {
        log_warn("MemTotal unreadable, smooth bufs -> %d", MP_SMOOTH_BUFS_SMALL_MEM);
        cached = MP_SMOOTH_BUFS_SMALL_MEM;
        return (unsigned int)cached;
    }

    cached = total_kb >= MP_MEM_LARGE_THRESHOLD_KB ? MP_SMOOTH_BUFS_LARGE_MEM
                                                   : MP_SMOOTH_BUFS_SMALL_MEM;
    if (cached > MP_SMOOTH_BUFS_MAX)
        cached = MP_SMOOTH_BUFS_MAX;
    log_info("MemTotal %lukB -> smooth bufs %d", total_kb, cached);
    return (unsigned int)cached;
}

/*
 * userdata 编码：低 8 位 = idx+1(0 表示无槽位)，第 9 位 = kind(解码
 * slot / rot buf)，再往上是会话代号。旋转切换会重建 rot 池，旧代 item
 * 迟到回流时只回收计数，不能碰新账本。
 */
static inline void *mp_userdata(mediaplayer_t *mp, int kind, int idx)
{
    return (void *)(uintptr_t)(((uintptr_t)mp->session_gen << 9) |
                               ((uintptr_t)kind << 8) |
                               (uint32_t)(idx + 1));
}

static int mp_trace = -1;
static inline int mp_trace_on(void)
{
    if (mp_trace < 0)
        mp_trace = getenv("MP_TRACE") != NULL;
    return mp_trace;
}

static inline int mp_stop_requested(mediaplayer_t *mp)
{
    int stop;

    pthread_rwlock_rdlock(&mp->thread.rwlock);
    stop = mp->thread.requested_stop;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    return stop;
}

/* 解码线程各等待点都要让路给控制请求，否则暂停时它们永远得不到执行 */
static inline int mp_control_pending(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;

    return mp_stop_requested(mp) ||
           atomic_load(&mp->seek_pending) ||
           atomic_load(&mp->rot_request) != p->cur_angle ||
           p->rot_resetup;
}

/* item 离开显示路径(回流或中途撤回)时的统一清账。只在解码线程调用。 */
static void mp_release_item(mediaplayer_t *mp, hal_display_queue_item_t *item)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    uintptr_t ud = (uintptr_t)item->userdata;
    int idx = (int)(ud & 0xff) - 1;
    int kind = (int)((ud >> 8) & 1);

    if (mp_trace_on())
        log_info("T R%d k%d g%d", idx, kind, (int)((ud >> 9) == mp->session_gen));
    if (idx >= 0 && (ud >> 9) == mp->session_gen) {
        if (kind == MP_UD_KIND_SLOT) {
            h264_dpb_set_on_screen(&p->dpb, idx, false);
            p->hold_slot[idx] = false;
        } else {
            if (p->hold_rot[idx]) {
                p->hold_rot[idx] = false;
                p->rot_free[p->rot_free_n++] = idx;
            }
        }
    }
    mp->items_in_flight--;
    free(item);
}

static void mp_reclaim_free_items(mediaplayer_t *mp)
{
    hal_display_queue_item_t *item;

    while (hal_display_try_dequeue_free_item(mp->hal_display,
                                             HAL_DISPLAY_LAYER_VIDEO,
                                             &item) == 0)
        mp_release_item(mp, item);
}

static void mp_pace_wait(mediaplayer_t *mp, long long *next)
{
    long long now = mp_get_now_us();

    if (!*next)
        *next = now;
    else if (now < *next)
        usleep(*next - now);
    else if (now > *next + 2 * 1000 * 1000) {
        log_warn("can't keep up, delay: %lld us", now - *next);
        *next = now;
    }
    *next += mp->frame_duration_us;
}

/*
 * 帧 item 工厂：占住 buffer + in_flight 记账。只在解码线程调用。
 * 直通帧(SLOT)在此设 on_screen；旋转帧的解码 slot 已在旋转完成时放手，
 * 这里押的是 rot buffer。
 */
static hal_display_queue_item_t *mp_make_frame_item(mediaplayer_t *mp,
                                                    uint32_t fb_id,
                                                    int kind, int idx)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    hal_display_queue_item_t *item = malloc(sizeof(*item));

    if (!item) {
        log_error("malloc err");
        return NULL;
    }
    memset(item, 0, sizeof(*item));
    item->type = HAL_DISPLAY_ITEM_FLIP_FB;
    item->fb_id = fb_id;
    item->userdata = mp_userdata(mp, kind, idx);
    item->on_heap = false;

    if (kind == MP_UD_KIND_SLOT) {
        h264_dpb_set_on_screen(&p->dpb, idx, true);
        h264_dpb_mark_displayed(&p->dpb, idx);
        p->hold_slot[idx] = true;
    } else {
        p->hold_rot[idx] = true;
    }
    if (mp_trace_on())
        log_info("T E%d k%d", idx, kind);
    mp->items_in_flight++;
    return item;
}

/* ---------- 视频层几何 ---------- */

static void mp_apply_geometry(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    int cw = mp->display_width, ch = mp->display_height;
    int dst_w, dst_h;

    if (p->cur_angle == 90 || p->cur_angle == 270) {
        cw = mp->display_height;
        ch = mp->display_width;
    }

    /* 等比铺满屏幕(aspect-fit)，DEFE 缩放 */
    dst_w = mp->screen_width;
    dst_h = (int)((int64_t)ch * mp->screen_width / cw);
    if (dst_h > mp->screen_height) {
        dst_h = mp->screen_height;
        dst_w = (int)((int64_t)cw * mp->screen_height / ch);
    }

    hal_display_set_layer_geometry(mp->hal_display, HAL_DISPLAY_LAYER_VIDEO,
                                   (mp->screen_width - dst_w) / 2,
                                   (mp->screen_height - dst_h) / 2,
                                   cw, ch, dst_w, dst_h);
    log_info("video geometry: %dx%d -> %dx%d angle=%d", cw, ch, dst_w, dst_h,
             p->cur_angle);
}

/* ---------- 旋转会话 ---------- */

static void mp_rot_teardown(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    unsigned int i;

    for (i = 0; i < ROT_MAX_CAP_BUFS; i++) {
        if (p->rot_fb_ids[i]) {
            hal_display_rm_fb(mp->hal_display, p->rot_fb_ids[i], p->rot_gems[i]);
            p->rot_fb_ids[i] = 0;
            p->rot_gems[i] = 0;
        }
    }
    if (p->rot_active) {
        rot_session_stop(&p->rot);
        p->rot_active = false;
    }
    p->rot_free_n = 0;
    memset(p->hold_rot, 0, sizeof(p->hold_rot));
}

static int mp_rot_setup(mediaplayer_t *mp, int angle)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    unsigned int i;

    if (!p->rot_fd_open) {
        if (rot_open(&p->rot) < 0)
            return -1;
        p->rot_fd_open = true;
    }

    if (rot_session_start(&p->rot, angle, mp->display_width,
                          mp->display_height, VP_ROT_BUFS) < 0)
        return -1;
    p->rot_active = true;

    for (i = 0; i < p->rot.cap_count; i++) {
        if (hal_display_import_dmabuf_fb(mp->hal_display,
                                         p->rot.cap[i].dmabuf_fd,
                                         p->rot.cap_width,
                                         p->rot.cap_height,
                                         p->rot.cap_bytesperline,
                                         p->rot.cap_uv_offset,
                                         &p->rot_fb_ids[i],
                                         &p->rot_gems[i]) < 0) {
            mp_rot_teardown(mp);
            return -1;
        }
    }

    p->rot_free_n = 0;
    for (i = 0; i < p->rot.cap_count; i++)
        p->rot_free[p->rot_free_n++] = (int)i;
    memset(p->hold_rot, 0, sizeof(p->hold_rot));
    return 0;
}

/*
 * 帧过旋转单元。返回 0 = *rot_idx 里是转好的帧；1 = 本帧放弃(硬件坏帧/
 * 控制请求插队)，调用方直接丢帧继续。
 */
static int mp_rot_frame(mediaplayer_t *mp, int slot, int *rot_idx)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    int rc;

    for (;;) {
        mp_reclaim_free_items(mp);
        if (p->rot_free_n > 0) {
            *rot_idx = p->rot_free[--p->rot_free_n];
            break;
        }
        if (mp_control_pending(mp))
            return 1;
        usleep(5 * 1000);
    }

    rc = rot_run(&p->rot, p->vdec.cap[slot].dmabuf_fd, *rot_idx);
    if (rc != 0) {
        p->rot_free[p->rot_free_n++] = *rot_idx;
        if (rc < 0) {
            /* 会话故障(DQBUF 失败等)：丢帧并请求循环顶部整重会话 */
            log_warn("rot session fault, scheduling rebuild");
            p->rot_resetup = true;
        }
        return 1;
    }
    return 0;
}

/* ---------- pacer 停走握手(seek/旋转切换期间) ---------- */

static void mp_pacer_hold_begin(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;

    atomic_store(&mp->pacer_hold, 1);
    while (!atomic_load(&mp->pacer_parked) &&
           atomic_load(&p->pacer_running))
        usleep(1000);
}

static void mp_pacer_hold_end(mediaplayer_t *mp)
{
    atomic_store(&mp->pacer_hold, 0);
}

/* 排空 smooth_q(pacer 已停住，队列双端都归解码线程) */
static void mp_drain_smooth_q(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    hal_display_queue_item_t *item;

    while (spsc_bq_try_pop(&p->smooth_q, (void **)&item) == 0)
        mp_release_item(mp, item);
}

/* 等显示端回流到只剩在屏那一帧(seek 期间画面定格，不黑屏) */
static void mp_wait_offscreen(mediaplayer_t *mp)
{
    int wait;

    for (wait = 0; wait < 100 && mp->items_in_flight > 1; wait++) {
        if (mp_stop_requested(mp))
            break;
        usleep(5 * 1000);
        mp_reclaim_free_items(mp);
    }
    if (mp->items_in_flight > 1)
        log_warn("offscreen wait timeout, %d items in flight",
                 mp->items_in_flight);
}

/* ---------- seek(解码线程执行) ---------- */

static void mp_do_seek(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    uint32_t target_ms = atomic_load(&mp->seek_target_ms);
    unsigned int idx;
    uint64_t ts_save;
    int i;

    mp_pacer_hold_begin(mp);
    mp_drain_smooth_q(mp);
    mp_wait_offscreen(mp);

    /*
     * DPB 推倒重来：seek 后首帧是 IDR，参考链从零建。ts_counter 必须保
     * 留——cedrus 按 QBUF 时间戳找参考帧(vb2_find_timestamp)，归零复用
     * 会撞上没重新入队的旧 capture buffer 的残留时间戳。
     */
    ts_save = p->dpb.ts_counter;
    h264_dpb_init(&p->dpb, p->dpb_capacity, p->dpb_max_ref,
                  p->dpb_max_frame_num, p->dpb_reorder);
    p->dpb.ts_counter = ts_save;
    /* 在屏冻结帧的槽在新 DPB 里立僵尸位：used 挡住 find_free_slot(光标
     * on_screen 挡不住，解码会写花正在扫描的 buffer)；回流时
     * set_on_screen(false) → free_if_done 正好把它收走 */
    for (i = 0; i < VDEC_MAX_CAP_BUFS; i++) {
        if (p->hold_slot[i]) {
            p->dpb.pics[i].used = true;
            p->dpb.pics[i].slot = i;
            p->dpb.pics[i].on_screen = true;
        }
    }
    if (p->rot_active) {
        p->rot_free_n = 0;
        for (i = 0; i < (int)p->rot.cap_count; i++)
            if (!p->hold_rot[i])
                p->rot_free[p->rot_free_n++] = i;
    }

    /* POC 状态跟着参考链清零；SPS/PPS 保留(avcC 不重解) */
    p->parser.have_prev = false;
    p->parser.prev_poc_msb = 0;
    p->parser.prev_poc_lsb = 0;
    p->parser.prev_frame_num = 0;
    p->parser.prev_frame_num_offset = 0;

    idx = (unsigned int)((uint64_t)target_ms * 1000 / mp->frame_duration_us);
    if (idx >= p->demux.samples_count)
        idx = p->demux.samples_count - 1;
    while (idx > 0 && !p->demux.samples[idx].sync)
        idx--;

    p->sample_idx = idx;
    p->pending_flush = false;
    p->wrap_pending = false;
    p->display_seq = 0;
    p->seek_base_ms = (uint64_t)idx * mp->frame_duration_us / 1000;
    atomic_store(&mp->pos_ms, (uint32_t)p->seek_base_ms);

    atomic_store(&mp->seek_pending, 0);
    if (atomic_load(&mp->paused))
        atomic_store(&mp->step_one, 1);
    mp_pacer_hold_end(mp);

    log_info("seek -> %ums (IDR sample %u)", (uint32_t)p->seek_base_ms, idx);
}

/* ---------- 旋转切换(解码线程执行) ---------- */

static void mp_do_rot_switch(mediaplayer_t *mp, int want)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    int i;

    p->rot_resetup = false;

    mp_pacer_hold_begin(mp);
    mp_drain_smooth_q(mp);
    mp_wait_offscreen(mp);

    /* plane 关掉后 buffer 不再被扫描，才能安全拆池/放 slot */
    hal_display_disable_layer_sync(mp->hal_display, HAL_DISPLAY_LAYER_VIDEO);

    for (i = 0; i < VDEC_MAX_CAP_BUFS; i++) {
        if (p->hold_slot[i]) {
            h264_dpb_set_on_screen(&p->dpb, i, false);
            p->hold_slot[i] = false;
        }
    }
    /* 层里残留的 curr_item 将在首个新帧翻页时回流，代号不符只计数 */
    mp->session_gen++;

    mp_rot_teardown(mp);
    if (want != 0 && mp_rot_setup(mp, want) < 0) {
        log_warn("rot setup for %d failed, falling back to 0", want);
        want = 0;
        atomic_store(&mp->rot_request, 0);
    }

    p->cur_angle = want;
    atomic_store(&mp->rot_angle, want);
    mp_apply_geometry(mp);
    /* plane 已 disable，暂停时也放一帧出去，不然黑屏到恢复播放 */
    if (atomic_load(&mp->paused))
        atomic_store(&mp->step_one, 1);
    mp_pacer_hold_end(mp);

    log_info("rotation -> %d", want);
}

/* ---------- pacer 线程 ---------- */

static void *mp_pacer_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    long long next_frame_time = 0;
    unsigned int outputs = 0;
    bool draining = false;

    atomic_store(&p->pacer_running, 1);
    log_info("==> mp_pacer Thread Started! dur=%uus smooth=%u",
             mp->frame_duration_us, p->smooth_bufs);

    while (1) {
        hal_display_queue_item_t *item = NULL;

        if (atomic_load(&mp->pacer_hold)) {
            atomic_store(&mp->pacer_parked, 1);
            next_frame_time = 0; /* 放行后档期从当下重起 */
            usleep(2 * 1000);
            continue;
        }
        atomic_store(&mp->pacer_parked, 0);

        if (!draining && atomic_load(&mp->paused) &&
            !atomic_load(&mp->step_one)) {
            /* 恢复播放时档期从当下重起，不追帧连发 */
            next_frame_time = 0;
            usleep(10 * 1000);
            continue;
        }

        if (!draining)
            mp_pace_wait(mp, &next_frame_time);

        /* 等帧期间也要响应 hold——seek 排空时 pacer 不能捞走旧帧 */
        for (;;) {
            int rc = spsc_bq_try_pop(&p->smooth_q, (void **)&item);

            if (rc == 0)
                break;
            if (rc != EAGAIN)
                goto out; /* 队列已关，stop 中 */
            if (atomic_load(&mp->pacer_hold)) {
                item = NULL;
                break;
            }
            usleep(2 * 1000);
        }
        if (!item)
            continue;

        if (!draining)
            draining = mp_stop_requested(mp);

        /* 时间轴：显示哪帧就报哪帧的时刻(暂停时自然冻结) */
        {
            uintptr_t ud = (uintptr_t)item->userdata;
            int idx = (int)(ud & 0xff) - 1;

            if (idx >= 0 && (ud >> 9) == mp->session_gen)
                atomic_store(&mp->pos_ms, ((ud >> 8) & 1) ? p->pts_rot[idx]
                                                          : p->pts_slot[idx]);
        }

        hal_display_enqueue_display_item(mp->hal_display,
                                         HAL_DISPLAY_LAYER_VIDEO, item);
        if (atomic_load(&mp->step_one))
            atomic_store(&mp->step_one, 0);

        if (!draining && ++outputs % 300 == 0)
            log_info("mp pace: out=%u lag=%lldms ring=%u/%u", outputs,
                     (long long)(mp_get_now_us() - next_frame_time) / 1000,
                     (unsigned int)spsc_bq_count(&p->smooth_q), p->smooth_bufs);
    }

out:
    atomic_store(&p->pacer_running, 0);
    log_info("==> mp_pacer Thread Ended!");
    return NULL;
}

/*
 * 解码一个 AU（单 slice）。返回:
 *  0 解码成功  1 非视频帧(跳过)  2 控制请求插队(AU 未消费，稍后重解)
 *  -1 错误(停播)
 */
static int mp_decode_au(mediaplayer_t *mp, unsigned int sample_idx,
                        struct h264_slice_hdr *hdr_out)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    const struct mp4_sample *sample = &p->demux.samples[sample_idx];
    const uint8_t *au = mp4_sample_data(&p->demux, sample_idx);
    unsigned int cursor = 0, vcl_count = 0;
    struct nalu n, vcl = { 0 };
    struct h264_poc poc;
    struct vdec_h264_ctrls ctrl;
    bool have_hdr = false;
    uint64_t ts;
    int slot, retry;

    if (!au) {
        log_error("sample %u out of range", sample_idx);
        return -1;
    }

    while (nalu_next_length_prefixed(au, sample->size,
                                     p->demux.nal_length_size, &cursor, &n)) {
        unsigned int t = nalu_h264_type(&n);

        if (t == H264_NAL_SPS || t == H264_NAL_PPS) {
            h264_parser_parse_param_nal(&p->parser, &n);
        } else if (t == H264_NAL_SLICE || t == H264_NAL_IDR) {
            vcl_count++;
            if (have_hdr)
                continue;
            if (h264_parser_parse_slice(&p->parser, &n, hdr_out) < 0) {
                log_error("slice parse failed @%u", sample_idx);
                return -1;
            }
            h264_parser_compute_poc(&p->parser, hdr_out, &poc);
            vcl = n;
            have_hdr = true;
        }
    }

    if (!have_hdr)
        return 1;

    /* 素材管线保证单 slice/帧；多 slice 明确报错，不静默花屏 */
    if (vcl_count > 1) {
        log_error("frame %u has %u slices, unsupported", sample_idx, vcl_count);
        return -1;
    }

    if (h264_parser_fill_controls(&p->parser, hdr_out, &poc, &ctrl) < 0) {
        log_error("fill_controls failed @%u", sample_idx);
        return -1;
    }

    long long t0 = mp_get_now_us(), t1, t2;

    /* 无空槽 = 在飞帧太多，等显示线程回流(每 vblank 一次) */
    for (retry = 0; retry < 100; retry++) {
        slot = h264_dpb_begin_frame(&p->dpb, hdr_out, &poc, &ts, &ctrl);
        if (slot >= 0)
            break;
        if (mp_control_pending(mp))
            break;
        usleep(5 * 1000);
        mp_reclaim_free_items(mp);
    }
    if (slot < 0) {
        /* compute_poc 对同一 slice 重算幂等，本 AU 留待控制处理后重解，
         * 不能跳过——GOP 中丢参考帧会花屏到下个 IDR */
        if (mp_control_pending(mp))
            return 2;
        log_error("no free capture slot @%u", sample_idx);
        return -1;
    }
    if (mp_trace_on())
        log_info("T D%d @%u", slot, sample_idx);
    t1 = mp_get_now_us();

    if (vdec_decode(&p->vdec, slot, ts, vcl.data, vcl.size, &ctrl) < 0) {
        log_error("decode failed @%u", sample_idx);
        h264_dpb_abort_frame(&p->dpb);
        return -1;
    }

    h264_dpb_end_frame(&p->dpb, hdr_out);
    t2 = mp_get_now_us();
    if (t2 - t0 > mp_slow_threshold_us(mp))
        log_warn("slow @%u: slot_wait=%lldus ve=%lldus size=%u",
                 sample_idx, t1 - t0, t2 - t1, vcl.size);
    return 0;
}

/*
 * 解码线程：出帧(POC 序) → (可选)旋转 → 交给 pacer。限速靠 smooth_q
 * 满时的反压(try_push 轮询，控制请求可插队)。
 */
static void *mp_decode_thread(void *param)
{
    mediaplayer_t *mp = (mediaplayer_t *)param;
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    bool fatal = false;
    int out;

    log_info("==> mp_decode Thread Started! dur=%uus samples=%u",
             mp->frame_duration_us, p->demux.samples_count);

    while (1) {
        if (mp_stop_requested(mp))
            break;
        if (atomic_load(&mp->seek_pending)) {
            mp_do_seek(mp);
            continue;
        }
        if (atomic_load(&mp->rot_request) != p->cur_angle || p->rot_resetup) {
            mp_do_rot_switch(mp, atomic_load(&mp->rot_request));
            continue;
        }

        mp_reclaim_free_items(mp);

        /*
         * 先备货再等收货：本轮的"喂 AU 直到吐出一帧"跑在下面出帧的阻塞点
         * (睡档期 / ring 满)之前，GOP 边界的重排回填消化在本来就要空等的
         * 窗口里(上游注释详述)。
         */

        /* GOP 边界(素材回绕)先按 flush 逐帧排空 DPB */
        if (p->pending_flush) {
            out = h264_dpb_next_output(&p->dpb, true);
            if (out < 0) {
                p->pending_flush = false;
                if (p->wrap_pending) {
                    p->wrap_pending = false;
                    p->seek_base_ms = 0;
                    p->display_seq = 0;
                }
            }
        } else {
            out = h264_dpb_next_output(&p->dpb, false);
        }

        /* 没有可显示帧就继续喂 AU，直到重排队列吐出一帧 */
        while (out < 0 && !p->pending_flush) {
            struct h264_slice_hdr hdr;
            int rc;

            if (mp_control_pending(mp))
                break; /* 回循环顶部消化控制请求 */

            if (p->sample_idx >= p->demux.samples_count) {
                /* EOS：排空后回 sample 0 循环（素材以 IDR 开头） */
                p->sample_idx = 0;
                p->pending_flush = true;
                p->wrap_pending = true;
                out = h264_dpb_next_output(&p->dpb, true);
                break;
            }

            /*
             * mid-stream IDR 前先按档期排空上一 GOP 押着的帧：IDR 的 POC
             * 复位为 0，直接喂会让旧帧(POC 最大)反排其后，GOP 边界帧序
             * 回跳(上游注释详述)。sync 采样 = IDR。
             */
            if (p->sample_idx > 0 && p->demux.samples[p->sample_idx].sync) {
                out = h264_dpb_next_output(&p->dpb, true);
                if (out >= 0)
                    break; /* 本档期先出旧帧，sample 不前进 */
            }

            long long d0 = mp_get_now_us();
            rc = mp_decode_au(mp, p->sample_idx, &hdr);
            long long decode_us = mp_get_now_us() - d0;
            if (decode_us > mp_slow_threshold_us(mp))
                log_warn("slow decode_au %lldus @%u",
                         decode_us, p->sample_idx);
            if (rc < 0) {
                fatal = true;
                break;
            }
            if (rc == 2)
                break; /* sample 不前进，循环顶部消化控制请求后重解 */
            p->sample_idx++;
            if (rc > 0)
                continue;

            out = h264_dpb_next_output(&p->dpb, false);
        }
        if (fatal)
            goto decode_error;

        if (out < 0)
            continue;

        uint32_t fb_id;
        int kind, idx;

        if (p->rot_active) {
            int rot_idx;

            int rc = mp_rot_frame(mp, out, &rot_idx);
            /* 无论成败，解码 slot 都已离开显示路径(显示押 rot buffer) */
            h264_dpb_mark_displayed(&p->dpb, out);
            if (rc)
                continue; /* 丢帧或控制请求插队 */
            kind = MP_UD_KIND_ROT;
            idx = rot_idx;
            fb_id = p->rot_fb_ids[rot_idx];
        } else {
            kind = MP_UD_KIND_SLOT;
            idx = out;
            fb_id = p->fb_ids[out];
        }

        hal_display_queue_item_t *item = mp_make_frame_item(mp, fb_id, kind, idx);
        if (!item)
            goto decode_error;

        {
            uint32_t pts = (uint32_t)(p->seek_base_ms +
                                      (uint64_t)p->display_seq *
                                          mp->frame_duration_us / 1000);
            if (kind == MP_UD_KIND_ROT)
                p->pts_rot[idx] = pts;
            else
                p->pts_slot[idx] = pts;
            p->display_seq++;
        }

        for (;;) {
            int rc = spsc_bq_try_push(&p->smooth_q, item);

            if (rc == 0)
                break;
            if (rc != EAGAIN) { /* 队列已关，stop 中 */
                mp_release_item(mp, item);
                goto exit_loop;
            }
            if (mp_control_pending(mp)) {
                /* 撤回本帧让路；seek/切旋转反正要排空 */
                mp_release_item(mp, item);
                break;
            }
            usleep(5 * 1000);
        }
    }

exit_loop:
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.state |= MEDIAPLAYER_DECODER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    log_info("==> mp_decode Thread Ended!");
    pthread_exit(NULL);
    return NULL;

decode_error:
    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.state |= MEDIAPLAYER_DECODER_ERROR | MEDIAPLAYER_DECODER_EXIT;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    log_error("==> mp_decode Thread Ended (error)!");
    pthread_exit(NULL);
    return NULL;
}

static void mp_close_session(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    unsigned int i;

    if (!mp->session_open)
        return;

    mp_rot_teardown(mp);
    for (i = 0; i < p->vdec.cap_count; i++) {
        if (p->fb_ids[i]) {
            hal_display_rm_fb(mp->hal_display, p->fb_ids[i], p->gem_handles[i]);
            p->fb_ids[i] = 0;
            p->gem_handles[i] = 0;
        }
    }
    vdec_close(&p->vdec);
    mp4_close(&p->demux);
    if (p->smooth_q_ready) {
        spsc_bq_destroy(&p->smooth_q);
        p->smooth_q_ready = false;
    }
    mp->session_open = false;
}

int mediaplayer_init(mediaplayer_t *mp, hal_display_t *hal_display,
                     int screen_width, int screen_height)
{
    memset(mp, 0, sizeof(*mp));

    mp->priv = calloc(1, sizeof(mp_dev_priv_t));
    if (!mp->priv) {
        log_error("mediaplayer priv alloc failed");
        return -1;
    }
    pthread_rwlock_init(&mp->thread.rwlock, NULL);
    atomic_store(&mp->running, 0);
    mp->hal_display = hal_display;
    mp->screen_width = screen_width;
    mp->screen_height = screen_height;

    log_info("==> mp Initalized!");
    return 0;
}

int mediaplayer_destroy(mediaplayer_t *mp)
{
    mp_dev_priv_t *p;

    if (!mp)
        return -1;
    p = (mp_dev_priv_t *)mp->priv;

    mediaplayer_stop(mp);
    if (p->rot_fd_open) {
        rot_close(&p->rot);
        p->rot_fd_open = false;
    }
    pthread_rwlock_destroy(&mp->thread.rwlock);
    free(mp->priv);
    mp->priv = NULL;

    return 0;
}

static int mp_prepare_and_spawn(mediaplayer_t *mp)
{
    mp_dev_priv_t *p = (mp_dev_priv_t *)mp->priv;
    char video_path[32], media_path[32];
    const struct h264_sps *sps = NULL;
    unsigned int cap_count, max_ref, reorder, max_frame_num;
    unsigned int i;
    int angle;

    if (mp4_open(&p->demux, mp->input_path) < 0) {
        log_error("mp4_open err: %s", mp->input_path);
        return -1;
    }
    mp->session_gen++;
    mp->session_open = true; /* demux 已开，之后统一走 close_session */

    if (p->demux.codec != MP4_CODEC_H264) {
        log_error("not an H264 mp4");
        goto error;
    }
    if (p->demux.max_sample_size > VDEC_OUTPUT_BUF_SIZE) {
        log_error("max sample %u exceeds output buffer", p->demux.max_sample_size);
        goto error;
    }

    h264_parser_init(&p->parser);
    if (h264_parser_parse_avcc(&p->parser, p->demux.extradata,
                               p->demux.extradata_size) < 0) {
        log_error("avcC parse err");
        goto error;
    }
    for (i = 0; i < 32 && !sps; i++)
        sps = h264_parser_get_sps(&p->parser, i);
    if (!sps) {
        log_error("no SPS in avcC");
        goto error;
    }
    if (!sps->frame_mbs_only_flag) {
        log_error("interlaced stream unsupported");
        goto error;
    }

    mp->frame_width = (sps->pic_width_in_mbs_minus1 + 1) * 16;
    mp->frame_height = (sps->pic_height_in_map_units_minus1 + 1) * 16;
    if (mp_set_display_size(mp, sps) < 0)
        goto error;

    mp->frame_duration_us = p->demux.frame_duration_us ?
                            p->demux.frame_duration_us : 33333;
    atomic_store(&mp->duration_ms,
                 (uint32_t)((uint64_t)p->demux.samples_count *
                            mp->frame_duration_us / 1000));
    atomic_store(&mp->pos_ms, 0);

    max_ref = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
    max_frame_num = 1 << (sps->log2_max_frame_num_minus4 + 4);
    if (sps->vui_reorder_valid) {
        /* refs 与重排共享 DPB。+4 = bump滞后1 + 入队未上屏1 + 在屏1 +
         * 解码中1(预算讨论见上游注释) */
        reorder = sps->vui_max_num_reorder_frames;
        cap_count = sps->vui_max_dec_frame_buffering + 4;
    } else {
        reorder = max_ref < VDEC_REORDER_DEPTH ? VDEC_REORDER_DEPTH : max_ref;
        cap_count = max_ref + reorder + 3;
    }
    {
        unsigned int cap_max =
            (unsigned int)mp->frame_width * mp->frame_height >=
                    VDEC_CAPTURE_LARGE_AREA ?
                VDEC_CAPTURE_BUF_MAX_LARGE : VDEC_CAPTURE_BUF_MAX_SMALL;
        if (cap_count < VDEC_CAPTURE_BUF_MIN)
            cap_count = VDEC_CAPTURE_BUF_MIN;
        if (cap_count > cap_max) {
            log_warn("capture need %u > budget %u (%dx%d), clamped;"
                     " 素材 ref/reorder 超预算可能饿槽",
                     cap_count, cap_max, mp->frame_width, mp->frame_height);
            cap_count = cap_max;
        }
        /* 平滑储备是解码正确性预算之外的额外格；pause/seek 都靠 pacer，
         * 所以本播放器至少押 1 格恒开 pacer */
        p->smooth_bufs = mp_smooth_bufs();
        if (p->smooth_bufs < 1)
            p->smooth_bufs = 1;
        if (cap_count + p->smooth_bufs > VDEC_MAX_CAP_BUFS)
            p->smooth_bufs = VDEC_MAX_CAP_BUFS - cap_count;
        cap_count += p->smooth_bufs;
    }

    if (spsc_bq_init(&p->smooth_q, p->smooth_bufs) != 0) {
        log_error("smooth queue init err");
        goto error;
    }
    p->smooth_q_ready = true;

    if (vdec_find_device(video_path, sizeof(video_path),
                         media_path, sizeof(media_path)) < 0)
        goto error;

    if (vdec_open(&p->vdec, video_path, media_path,
                  mp->frame_width, mp->frame_height,
                  cap_count, VDEC_OUTPUT_BUF_COUNT, VDEC_OUTPUT_BUF_SIZE) < 0)
        goto error;
    p->vdec.slow_threshold_us = mp_slow_threshold_us(mp);

    for (i = 0; i < p->vdec.cap_count; i++) {
        if (hal_display_import_dmabuf_fb(mp->hal_display,
                                         p->vdec.cap[i].dmabuf_fd,
                                         p->vdec.cap_width,
                                         p->vdec.cap_height,
                                         p->vdec.cap_bytesperline,
                                         p->vdec.cap_uv_offset,
                                         &p->fb_ids[i],
                                         &p->gem_handles[i]) < 0)
            goto error;
    }

    p->dpb_capacity = (int)cap_count;
    p->dpb_max_ref = (int)max_ref;
    p->dpb_max_frame_num = (int)max_frame_num;
    p->dpb_reorder = (int)reorder;
    h264_dpb_init(&p->dpb, cap_count, max_ref, max_frame_num, reorder);

    memset(p->hold_slot, 0, sizeof(p->hold_slot));
    p->sample_idx = 0;
    p->pending_flush = false;
    p->wrap_pending = false;
    p->seek_base_ms = 0;
    p->display_seq = 0;

    /* 横屏素材自动立起来；方向宏顺逆待真机验证(config.h) */
    angle = mp->display_width > mp->display_height ? VP_ROT_CW_ANGLE : 0;
    if (angle != 0 && mp_rot_setup(mp, angle) < 0) {
        log_warn("cedrus-rotate unavailable, playing unrotated");
        angle = 0;
    }
    p->cur_angle = angle;
    atomic_store(&mp->rot_request, angle);
    atomic_store(&mp->rot_angle, angle);
    atomic_store(&mp->paused, 0);
    atomic_store(&mp->step_one, 0);
    atomic_store(&mp->seek_pending, 0);
    atomic_store(&mp->pacer_hold, 0);
    atomic_store(&mp->pacer_parked, 0);
    mp_apply_geometry(mp);

    log_info("vdec: coded=%ux%u display=%dx%d max_ref=%u cap_bufs=%u(smooth %u)"
             " dur=%uus angle=%d",
             mp->frame_width, mp->frame_height,
             mp->display_width, mp->display_height, max_ref, cap_count,
             p->smooth_bufs, mp->frame_duration_us, angle);

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.state = 0;
    mp->thread.requested_stop = 0;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    atomic_store(&mp->running, 1);

    /* pacer 先起：它阻塞等首帧，解码线程一出帧就有人接 */
    if (pthread_create(&p->pacer_thread, NULL, mp_pacer_thread, mp) != 0) {
        log_error("pacer thread create err");
        atomic_store(&mp->running, 0);
        goto error;
    }
    p->pacer_started = true;

    if (pthread_create(&mp->decode_thread, NULL, mp_decode_thread, mp) != 0) {
        log_error("decode thread create err");
        atomic_store(&mp->running, 0);
        goto error;
    }

    return 0;

error:
    if (p->pacer_started) {
        spsc_bq_close(&p->smooth_q);
        pthread_join(p->pacer_thread, NULL);
        p->pacer_started = false;
    }
    mp_close_session(mp);
    return -1;
}

int mediaplayer_play_video(mediaplayer_t *mp, const char *file)
{
    if (!mp || !file) {
        log_error("invalid params");
        return -1;
    }

    if (atomic_load(&mp->running)) {
        log_error("mediaplayer is running");
        return -1;
    }

    snprintf(mp->input_path, sizeof(mp->input_path), "%s", file);

    return mp_prepare_and_spawn(mp);
}

int mediaplayer_stop(mediaplayer_t *mp)
{
    mp_dev_priv_t *p;
    int wait;

    if (!mp)
        return -1;

    if (!atomic_load(&mp->running))
        return 0;
    p = (mp_dev_priv_t *)mp->priv;

    pthread_rwlock_wrlock(&mp->thread.rwlock);
    mp->thread.requested_stop = 1;
    pthread_rwlock_unlock(&mp->thread.rwlock);

    /* 暂停/hold 都放开，pacer 的各个 gate 才走得到出口 */
    atomic_store(&mp->paused, 0);
    atomic_store(&mp->pacer_hold, 0);

    /* 关队列放走可能堵着的两边；pacer 把残帧冲进显示队列(不再定速)后
     * 收工，item 照常经 free_queue 回流，下面的 in_flight 等待才收敛 */
    if (p->smooth_q_ready)
        spsc_bq_close(&p->smooth_q);

    pthread_join(mp->decode_thread, NULL);
    if (p->pacer_started) {
        pthread_join(p->pacer_thread, NULL);
        p->pacer_started = false;
    }
    atomic_store(&mp->running, 0);

    // 等积压的 FLIP 回流，只剩屏上帧(curr)
    for (wait = 0; wait < 40 && mp->items_in_flight > 1; wait++) {
        usleep(10 * 1000);
        mp_reclaim_free_items(mp);
    }
    if (mp->items_in_flight > 2)
        log_warn("stop: %d frame items still in flight", mp->items_in_flight);

    // 关掉 video plane(最底层，露出 DEBE 黑背景)，此后 RmFB 碰不到在屏 fb
    hal_display_disable_layer_sync(mp->hal_display, HAL_DISPLAY_LAYER_VIDEO);

    mp_close_session(mp);

    return 0;
}

mp_status_t mediaplayer_get_status(mediaplayer_t *mp)
{
    int state;

    if (!mp)
        return MP_STATUS_ERROR;

    if (!atomic_load(&mp->running))
        return MP_STATUS_STOPPED;

    pthread_rwlock_rdlock(&mp->thread.rwlock);
    state = mp->thread.state;
    pthread_rwlock_unlock(&mp->thread.rwlock);
    if (state & MEDIAPLAYER_DECODER_ERROR)
        return MP_STATUS_ERROR;

    return atomic_load(&mp->paused) ? MP_STATUS_PAUSED : MP_STATUS_PLAYING;
}

void mediaplayer_set_paused(mediaplayer_t *mp, bool paused)
{
    if (!mp)
        return;
    atomic_store(&mp->paused, paused ? 1 : 0);
}

bool mediaplayer_get_paused(mediaplayer_t *mp)
{
    return mp && atomic_load(&mp->paused);
}

int mediaplayer_seek_ms(mediaplayer_t *mp, uint32_t target_ms)
{
    uint32_t dur;

    if (!mp || !atomic_load(&mp->running))
        return -1;

    dur = atomic_load(&mp->duration_ms);
    if (dur && target_ms >= dur)
        target_ms = dur ? dur - 1 : 0;
    atomic_store(&mp->seek_target_ms, target_ms);
    atomic_store(&mp->seek_pending, 1);
    return 0;
}

int mediaplayer_set_rotation(mediaplayer_t *mp, int angle)
{
    if (!mp || !atomic_load(&mp->running))
        return -1;
    if (angle != 0 && angle != 90 && angle != 180 && angle != 270)
        return -1;
    atomic_store(&mp->rot_request, angle);
    return 0;
}

int mediaplayer_get_rotation(mediaplayer_t *mp)
{
    return mp ? atomic_load(&mp->rot_angle) : 0;
}

uint32_t mediaplayer_get_position_ms(mediaplayer_t *mp)
{
    return mp ? atomic_load(&mp->pos_ms) : 0;
}

uint32_t mediaplayer_get_duration_ms(mediaplayer_t *mp)
{
    return mp ? atomic_load(&mp->duration_ms) : 0;
}
