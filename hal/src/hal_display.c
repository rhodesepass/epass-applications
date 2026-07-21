#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "hal_display.h"
#include "log.h"
#include "spsc_queue.h"

static void hal_display_wait_for_vsync(hal_display_t *hal_display){
    hal_display->blank.request.type = DRM_VBLANK_RELATIVE;
    hal_display->blank.request.sequence = 1;
    if (drmWaitVBlank(hal_display->fd, (drmVBlankPtr) &hal_display->blank)) {
      log_error("drmWaitVBlank failed %s(%d)", strerror(errno), errno);
      usleep(20 * 1000); // 失败也维持节奏，防止立即返回时空转烧 CPU
    }
}

// alpha 属性 0..0xFFFF；255*0x101 = 0xFFFF 正好回到不透明(像素 alpha)
static inline uint64_t alpha_to_prop(uint8_t alpha){
    return (uint64_t)alpha * 0x101;
}

static uint32_t hal_display_find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name){
    drmModeObjectProperties *props;
    uint32_t id = 0;
    int i;

    props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props)
        return 0;
    for (i = 0; i < (int)props->count_props && !id; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            continue;
        if (!strcmp(prop->name, name))
            id = prop->prop_id;
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

// 内核关掉 DRM_FBDEV_EMULATION 后没有 fbcon 替我们做开机 modeset，
// CRTC 是灭的：plane commit 全被拒、drmWaitVBlank 每次卡 3s 超时。
// 检测到 CRTC 未点亮就自己做一次(mode 取 connector 首选模式)。
static int hal_display_ensure_crtc_active(hal_display_t *hal_display){
    drmModeCrtc *crtc;
    drmModeAtomicReq *req;
    uint32_t conn_crtc_prop, crtc_mode_prop, crtc_active_prop, blob_id;
    bool active;
    int ret;

    crtc = drmModeGetCrtc(hal_display->fd, hal_display->crtc_id);
    active = crtc && crtc->mode_valid;
    drmModeFreeCrtc(crtc);
    if (active)
        return 0;

    log_info("CRTC is off (no fbcon modeset), performing initial modeset");

    conn_crtc_prop = hal_display_find_prop(hal_display->fd, hal_display->conn_id,
                                           DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    crtc_mode_prop = hal_display_find_prop(hal_display->fd, hal_display->crtc_id,
                                           DRM_MODE_OBJECT_CRTC, "MODE_ID");
    crtc_active_prop = hal_display_find_prop(hal_display->fd, hal_display->crtc_id,
                                             DRM_MODE_OBJECT_CRTC, "ACTIVE");
    if (!conn_crtc_prop || !crtc_mode_prop || !crtc_active_prop) {
        log_error("modeset properties missing");
        return -1;
    }

    ret = drmModeCreatePropertyBlob(hal_display->fd, &hal_display->conn->modes[0],
                                    sizeof(hal_display->conn->modes[0]), &blob_id);
    if (ret < 0) {
        log_error("create mode blob failed %s(%d)", strerror(errno), errno);
        return -1;
    }

    req = drmModeAtomicAlloc();
    if (!req)
        return -1;
    drmModeAtomicAddProperty(req, hal_display->conn_id, conn_crtc_prop, hal_display->crtc_id);
    drmModeAtomicAddProperty(req, hal_display->crtc_id, crtc_mode_prop, blob_id);
    drmModeAtomicAddProperty(req, hal_display->crtc_id, crtc_active_prop, 1);

    ret = drmModeAtomicCommit(hal_display->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    if (ret < 0)
        log_error("initial modeset commit err %s(%d)", strerror(errno), errno);
    return ret;
}

static int hal_display_discover_plane_props(hal_display_t *hal_display, int layer_id){
    hal_display_plane_prop_ids_t *p = &hal_display->plane_props[layer_id];
    drmModeObjectProperties *props;
    int i;

    memset(p, 0, sizeof(*p));

    props = drmModeObjectGetProperties(hal_display->fd,
                                       hal_display->plane_ids[layer_id],
                                       DRM_MODE_OBJECT_PLANE);
    if (!props) {
        log_error("get plane %d properties failed", layer_id);
        return -1;
    }

    for (i = 0; i < (int)props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(hal_display->fd, props->props[i]);
        if (!prop)
            continue;
        if      (!strcmp(prop->name, "FB_ID"))   p->fb_id   = prop->prop_id;
        else if (!strcmp(prop->name, "CRTC_ID")) p->crtc_id = prop->prop_id;
        else if (!strcmp(prop->name, "SRC_X"))   p->src_x   = prop->prop_id;
        else if (!strcmp(prop->name, "SRC_Y"))   p->src_y   = prop->prop_id;
        else if (!strcmp(prop->name, "SRC_W"))   p->src_w   = prop->prop_id;
        else if (!strcmp(prop->name, "SRC_H"))   p->src_h   = prop->prop_id;
        else if (!strcmp(prop->name, "CRTC_X"))  p->crtc_x  = prop->prop_id;
        else if (!strcmp(prop->name, "CRTC_Y"))  p->crtc_y  = prop->prop_id;
        else if (!strcmp(prop->name, "CRTC_W"))  p->crtc_w  = prop->prop_id;
        else if (!strcmp(prop->name, "CRTC_H"))  p->crtc_h  = prop->prop_id;
        else if (!strcmp(prop->name, "alpha"))   p->alpha   = prop->prop_id;
        else if (!strcmp(prop->name, "zpos"))    p->zpos    = prop->prop_id;
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);

    if (!p->fb_id || !p->crtc_id || !p->crtc_x || !p->crtc_y) {
        log_error("plane %d missing required properties", layer_id);
        return -1;
    }
    return 0;
}

// 每层每轮 drain 到的帧类 item。free_queue 回收必须推迟到 commit 返回之后：
// 提前回收的话，解码侧会在旧帧还在扫描时就把该 capture buffer 重新 QBUF
// 给 cedrus 覆写——屏上直接花。
#define HAL_DISPLAY_DRAIN_MAX 16
typedef struct {
    hal_display_queue_item_t *items[HAL_DISPLAY_DRAIN_MAX];
    int n;
} drained_frames_t;

static inline int64_t dw_now_us(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void* hal_display_display_thread(void *arg){
    hal_display_t *hal_display = (hal_display_t *)arg;
    int ret;
    // video 层节拍诊断：commit 间隔与跳帧/空转分布，量化"抖动"
    struct {
        uint32_t commits, frames, empties;
        int64_t last_us, sum_us, max_us;
    } vstat = { 0 };

    log_info("==> hal_display thread Started!");

    // 节奏：有活时直接阻塞 commit（commit 本身按 vblank 节拍返回，
    // 背靠背即每 vblank 一次）；空转时用 drmWaitVBlank 兜底等待。
    while(atomic_load(&hal_display->thread_running)){
        drmModeAtomicReq *req = NULL;
        drained_frames_t drained[4] = { 0 };

        for(int i = 0; i < 4; i++){
            hal_display_layer_t* layer = &hal_display->layer[i];
            hal_display_plane_prop_ids_t *p = &hal_display->plane_props[i];
            if(!layer->used)
                continue;
            hal_display_queue_item_t* item;
            while(spsc_bq_try_pop(&layer->display_queue, (void**)&item) == 0){
                switch(item->type){
                case HAL_DISPLAY_ITEM_FLIP_FB:
                    if(drained[i].n < HAL_DISPLAY_DRAIN_MAX){
                        drained[i].items[drained[i].n++] = item;
                    }
                    else{
                        // 不可能路径(display_queue 容量即 16)，直接判为已跳过
                        spsc_bq_push(&layer->free_queue, item);
                    }
                    break;
                case HAL_DISPLAY_ITEM_SET_COORD:
                    if(!req) req = drmModeAtomicAlloc();
                    drmModeAtomicAddProperty(req, hal_display->plane_ids[i],
                                             p->crtc_x, (uint64_t)(int64_t)item->x);
                    drmModeAtomicAddProperty(req, hal_display->plane_ids[i],
                                             p->crtc_y, (uint64_t)(int64_t)item->y);
                    if(item->on_heap) free(item);
                    break;
                case HAL_DISPLAY_ITEM_SET_ALPHA:
                    if(p->alpha){
                        if(!req) req = drmModeAtomicAlloc();
                        drmModeAtomicAddProperty(req, hal_display->plane_ids[i],
                                                 p->alpha, alpha_to_prop(item->alpha));
                    }
                    else{
                        log_error("layer %d has no alpha property", i);
                    }
                    if(item->on_heap) free(item);
                    break;
                }
            }

            // 多个待翻帧只上最后一帧(60fps 素材 vs 40Hz 屏，跳帧在此发生)
            if(drained[i].n > 0){
                if(!req) req = drmModeAtomicAlloc();
                drmModeAtomicAddProperty(req, hal_display->plane_ids[i], p->fb_id,
                                         drained[i].items[drained[i].n - 1]->fb_id);
                // 惰性挂载：plane 处于 disabled 时随首帧一并启用
                if(layer->needs_full_mount){
                    uint32_t pl = hal_display->plane_ids[i];
                    drmModeAtomicAddProperty(req, pl, p->crtc_id, hal_display->crtc_id);
                    drmModeAtomicAddProperty(req, pl, p->src_x, 0);
                    drmModeAtomicAddProperty(req, pl, p->src_y, 0);
                    drmModeAtomicAddProperty(req, pl, p->src_w, (uint64_t)layer->geo_src_w << 16);
                    drmModeAtomicAddProperty(req, pl, p->src_h, (uint64_t)layer->geo_src_h << 16);
                    drmModeAtomicAddProperty(req, pl, p->crtc_x, (uint64_t)(int64_t)layer->geo_x);
                    drmModeAtomicAddProperty(req, pl, p->crtc_y, (uint64_t)(int64_t)layer->geo_y);
                    drmModeAtomicAddProperty(req, pl, p->crtc_w, layer->geo_dst_w);
                    drmModeAtomicAddProperty(req, pl, p->crtc_h, layer->geo_dst_h);
                    // z-order = layer_id (UI 2 > overlay 1 > video 0)。不设的话
                    // 全靠驱动 reset 默认值，overlay(唯一带 alpha 层)一旦归一化后
                    // zpos 抬到 UI 之上，就会独占高 pipe 盖住 UI。见 mount_layer_rect。
                    if(p->zpos)
                        drmModeAtomicAddProperty(req, pl, p->zpos, (uint64_t)i);
                }
            }
        }

        if(req){
            bool has_video = drained[HAL_DISPLAY_LAYER_VIDEO].n > 0;
            int64_t c0, cdt;

            // 阻塞 commit：返回 = 新帧已在 vblank latch、旧帧离屏，被换下的
            // curr 立即回收——video 层在屏只押 1 格 capture buffer(32MB 机
            // 型的省钱点)。代价是本线程每帧最多干等一个 vblank，同队列的
            // UI/overlay item 顺延；它们反正也要等下一个 vblank 才生效。
            c0 = dw_now_us();
            pthread_mutex_lock(&hal_display->commit_mutex);
            ret = drmModeAtomicCommit(hal_display->fd, req, 0, NULL);
            pthread_mutex_unlock(&hal_display->commit_mutex);
            cdt = dw_now_us() - c0;
            if(cdt > 60000)
                log_warn("slow commit %lldus (video frames=%d)",
                         (long long)cdt, drained[HAL_DISPLAY_LAYER_VIDEO].n);
            if(ret < 0){
                log_error("drmModeAtomicCommit failed %s(%d)", strerror(errno), errno);
            }
            drmModeAtomicFree(req);

            if(ret == 0 && has_video){
                static int dw_trace = -1;
                if(dw_trace < 0)
                    dw_trace = getenv("MP_TRACE") != NULL;
                if(dw_trace){
                    drained_frames_t *dv = &drained[HAL_DISPLAY_LAYER_VIDEO];
                    log_info("T C%d skip%d",
                             (int)(((uintptr_t)dv->items[dv->n - 1]->userdata) & 0xff) - 1,
                             dv->n - 1);
                }
                int64_t now = dw_now_us();
                if(vstat.last_us){
                    int64_t dt = now - vstat.last_us;
                    vstat.sum_us += dt;
                    if(dt > vstat.max_us) vstat.max_us = dt;
                }
                vstat.last_us = now;
                vstat.commits++;
                vstat.frames += drained[HAL_DISPLAY_LAYER_VIDEO].n;
                if(vstat.commits == 400){
                    log_info("vstat: 400 commits %u frames(skip %u) empty=%u avg=%lldus max=%lldus",
                             vstat.frames, vstat.frames - vstat.commits,
                             vstat.empties, (long long)vstat.sum_us / 399,
                             (long long)vstat.max_us);
                    memset(&vstat, 0, sizeof(vstat));
                    vstat.last_us = now;
                }
            }

            for(int i = 0; i < 4; i++){
                hal_display_layer_t* layer = &hal_display->layer[i];
                int n = drained[i].n;
                if(n == 0)
                    continue;
                if(ret == 0){
                    layer->needs_full_mount = false;
                    // 被跳过的 n-1 帧从未递交给硬件，立即回收
                    for(int k = 0; k < n - 1; k++)
                        spsc_bq_push(&layer->free_queue, drained[i].items[k]);
                    // 阻塞 commit 已返回 = 旧 curr 离屏，直换并立即回收
                    if(layer->curr_item)
                        spsc_bq_push(&layer->free_queue, layer->curr_item);
                    layer->curr_item = drained[i].items[n - 1];
                }
                else{
                    // commit 被拒：屏上还是旧 curr，本轮所有帧原样回收
                    for(int k = 0; k < n; k++)
                        spsc_bq_push(&layer->free_queue, drained[i].items[k]);
                }
            }
        }
        else{
            if(vstat.last_us) vstat.empties++;
            hal_display_wait_for_vsync(hal_display);
        }
    }

    log_info("==> hal_display thread Ended!");
    return NULL;
}

// 惰性启动显示线程。首次 enqueue 时起；纯同步消费者(只 mount_layer)永不调到。
static int hal_display_ensure_display_thread(hal_display_t *hal_display){
    if(atomic_load(&hal_display->thread_started))
        return 0;
    int ret = 0;
    pthread_mutex_lock(&hal_display->commit_mutex);
    if(!atomic_load(&hal_display->thread_started)){
        atomic_store(&hal_display->thread_running, 1);
        if(pthread_create(&hal_display->display_thread, NULL,
                          hal_display_display_thread, hal_display) != 0){
            log_error("Failed to create display thread");
            atomic_store(&hal_display->thread_running, 0);
            ret = -1;
        }
        else{
            atomic_store(&hal_display->thread_started, 1);
        }
    }
    pthread_mutex_unlock(&hal_display->commit_mutex);
    return ret;
}

int hal_display_enqueue_display_item(hal_display_t *hal_display,int layer_id,hal_display_queue_item_t* item){
    if(hal_display_ensure_display_thread(hal_display) < 0)
        return -1;
    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    return spsc_bq_push(&layer->display_queue, item);
}

int hal_display_dequeue_free_item(hal_display_t *hal_display,int layer_id,hal_display_queue_item_t** out_item){
    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    return spsc_bq_pop(&layer->free_queue, (void**)out_item);
}

int hal_display_try_dequeue_free_item(hal_display_t *hal_display,int layer_id,hal_display_queue_item_t** out_item){
    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    return spsc_bq_try_pop(&layer->free_queue, (void**)out_item);
}

int hal_display_set_layer_coord(hal_display_t *hal_display,int layer_id,int x,int y){
    hal_display_queue_item_t *item = malloc(sizeof(hal_display_queue_item_t));
    if(item == NULL){
        log_error("failed to allocate memory");
        return -1;
    }
    memset(item, 0, sizeof(*item));
    item->type = HAL_DISPLAY_ITEM_SET_COORD;
    item->x = (int16_t)x;
    item->y = (int16_t)y;
    item->on_heap = true;
#ifndef APP_RELEASE
    log_trace("drm coord y:%d,x:%d",y,x);
#endif // APP_RELEASE
    return hal_display_enqueue_display_item(hal_display, layer_id, item);
}

bool hal_display_layer_can_fade(const hal_display_t *hal_display,int layer_id){
    return layer_id >= 0 && layer_id < 4 &&
           hal_display->plane_ids[layer_id] &&
           hal_display->plane_props[layer_id].alpha;
}

int hal_display_set_layer_alpha(hal_display_t *hal_display,int layer_id,uint8_t alpha){
    if(!hal_display_layer_can_fade(hal_display, layer_id))
        return -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req)
        return -1;
    drmModeAtomicAddProperty(req, hal_display->plane_ids[layer_id],
                             hal_display->plane_props[layer_id].alpha,
                             alpha_to_prop(alpha));
    pthread_mutex_lock(&hal_display->commit_mutex);
    int rc = drmModeAtomicCommit(hal_display->fd, req, 0, NULL);
    pthread_mutex_unlock(&hal_display->commit_mutex);
    drmModeAtomicFree(req);
    return rc;
}

int hal_display_mount_layer_alpha(hal_display_t *hal_display,int layer_id,int x,int y,hal_buffer_t *buf,uint8_t alpha){
    hal_display_plane_prop_ids_t *p = &hal_display->plane_props[layer_id];
    uint32_t plane_id = hal_display->plane_ids[layer_id];
    if(!plane_id)
        return -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req)
        return -1;
    drmModeAtomicAddProperty(req, plane_id, p->crtc_id, hal_display->crtc_id);
    drmModeAtomicAddProperty(req, plane_id, p->fb_id, buf->fb_id);
    drmModeAtomicAddProperty(req, plane_id, p->src_x, 0);
    drmModeAtomicAddProperty(req, plane_id, p->src_y, 0);
    drmModeAtomicAddProperty(req, plane_id, p->src_w, (uint64_t)buf->width << 16);
    drmModeAtomicAddProperty(req, plane_id, p->src_h, (uint64_t)buf->height << 16);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_x, (uint64_t)(int64_t)x);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_y, (uint64_t)(int64_t)y);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_w, buf->width);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_h, buf->height);
    // zpos 与 fb/alpha 同一次 commit：挂上瞬间层序与 alpha 都到位，不闪帧。
    if(p->zpos)
        drmModeAtomicAddProperty(req, plane_id, p->zpos, (uint64_t)layer_id);
    if(p->alpha)
        drmModeAtomicAddProperty(req, plane_id, p->alpha, alpha_to_prop(alpha));
    pthread_mutex_lock(&hal_display->commit_mutex);
    int rc = drmModeAtomicCommit(hal_display->fd, req, 0, NULL);
    pthread_mutex_unlock(&hal_display->commit_mutex);
    drmModeAtomicFree(req);
    return rc;
}

int hal_display_init(hal_display_t *hal_display){
    int ret;

    memset(hal_display, 0, sizeof(hal_display_t));

    hal_display->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (hal_display->fd < 0) {
        log_error("open /dev/dri/card0 failed");
        return -1;
    }

    // 上一实例还没死透时 open 拿不到 master，之后所有 atomic 全 EACCES。
    // 短暂重试等旧实例放手；拿不到也继续跑(仅报警)，别整只 app 起不来
    for (int tries = 0; drmSetMaster(hal_display->fd) != 0; tries++) {
        if (tries >= 20) {
            log_error("cannot become DRM master: %s (stale instance alive?)",
                      strerror(errno));
            break;
        }
        usleep(100 * 1000);
    }

    ret = drmSetClientCap(hal_display->fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if(ret) {
        log_error("No atomic modesetting support: %s", strerror(errno));
        close(hal_display->fd);
        return -1;
    }

    hal_display->res = drmModeGetResources(hal_display->fd);
    if (!hal_display->res || hal_display->res->count_crtcs == 0 || hal_display->res->count_connectors == 0) {
        log_error("drmModeGetResources failed or no CRTCs/connectors");
        close(hal_display->fd);
        return -1;
    }
    hal_display->crtc_id = hal_display->res->crtcs[0];
    hal_display->conn_id = hal_display->res->connectors[0];

    ret = drmSetClientCap(hal_display->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret) {
      log_error("failed to set client cap\n");
      drmModeFreeResources(hal_display->res);
      close(hal_display->fd);
      return -1;
    }
    hal_display->plane_res = drmModeGetPlaneResources(hal_display->fd);
    if (!hal_display->plane_res) {
        log_error("drmModeGetPlaneResources failed");
        drmModeFreeResources(hal_display->res);
        close(hal_display->fd);
        return -1;
    }
    log_info("Available Plane Count: %d", hal_display->plane_res->count_planes);

    for(uint32_t i = 0; i < 4 && i < hal_display->plane_res->count_planes; i++){
        hal_display->plane_ids[i] = hal_display->plane_res->planes[i];
        if(hal_display_discover_plane_props(hal_display, i) < 0){
            drmModeFreePlaneResources(hal_display->plane_res);
            drmModeFreeResources(hal_display->res);
            close(hal_display->fd);
            return -1;
        }
    }

    hal_display->conn = drmModeGetConnector(hal_display->fd, hal_display->conn_id);
    if (!hal_display->conn) {
        log_error("drmModeGetConnector failed");
        drmModeFreePlaneResources(hal_display->plane_res);
        drmModeFreeResources(hal_display->res);
        close(hal_display->fd);
        return -1;
    }

    log_info("Connector Name: %s, %dx%d, Refresh Rate: %d",
        hal_display->conn->modes[0].name, hal_display->conn->modes[0].vdisplay, hal_display->conn->modes[0].hdisplay,
        hal_display->conn->modes[0].vrefresh);

    if (hal_display_ensure_crtc_active(hal_display) < 0) {
        log_error("CRTC bring-up failed, display will not work");
        drmModeFreeConnector(hal_display->conn);
        drmModeFreePlaneResources(hal_display->plane_res);
        drmModeFreeResources(hal_display->res);
        close(hal_display->fd);
        return -1;
    }

    hal_display->blank.request.type = DRM_VBLANK_RELATIVE;
    hal_display->blank.request.sequence = 1;

    pthread_mutex_init(&hal_display->commit_mutex, NULL);

    // 显示线程惰性启动(首次 enqueue 才起)。纯同步消费者不背这个线程。
    atomic_store(&hal_display->thread_running, 0);
    atomic_store(&hal_display->thread_started, 0);
    return 0;
}

int hal_display_stop(hal_display_t *hal_display){
    if(atomic_load(&hal_display->thread_started)){
        atomic_store(&hal_display->thread_running, 0);
        log_info("wait for display thread to finish");
        pthread_join(hal_display->display_thread, NULL);
        log_info("display thread finished");
        atomic_store(&hal_display->thread_started, 0);
    }
    return 0;
}

int hal_display_destroy(hal_display_t *hal_display){
    // 先停线程再释放资源（原实现先 close fd，线程还在用）
    hal_display_stop(hal_display);

    for(int i = 0; i < 4; i++){
        hal_display_destroy_layer(hal_display, i);
    }

    drmModeFreeConnector(hal_display->conn);
    drmModeFreePlaneResources(hal_display->plane_res);
    drmModeFreeResources(hal_display->res);
    pthread_mutex_destroy(&hal_display->commit_mutex);
    close(hal_display->fd);

    return 0;
}

int hal_display_display_size(const hal_display_t *hal_display,int *width,int *height){
    if(!hal_display || !hal_display->conn || hal_display->conn->count_modes <= 0)
        return -1;
    if(width)  *width  = hal_display->conn->modes[0].hdisplay;
    if(height) *height = hal_display->conn->modes[0].vdisplay;
    return 0;
}

static int hal_display_create_buffer_object(int fd,hal_buffer_t* bo,int width,int height,hal_display_layer_mode_t mode){
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;
    uint32_t handles[4], pitches[4], offsets[4];
    uint64_t modifiers[4];
    int ret;

    memset(&creq, 0, sizeof(struct drm_mode_create_dumb));
    if(mode == HAL_DISPLAY_LAYER_MODE_MB32_NV12){
        creq.width = width;
        creq.height = height * 3 / 2;
        creq.bpp = 8;
    }
    else if(mode == HAL_DISPLAY_LAYER_MODE_RGB565 || mode == HAL_DISPLAY_LAYER_MODE_ARGB1555){
        creq.width = width;
        creq.height = height;
        creq.bpp = 16;
    }
    else if(mode == HAL_DISPLAY_LAYER_MODE_ARGB8888){
        creq.width = width;
        creq.height = height;
        creq.bpp = 32;
    }
    else{
        log_error("invalid layer mode");
        return -1;
    }


    ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    if (ret < 0) {
      log_error("cannot create dumb buffer (%d): %m", errno);
      return -errno;
    }

    memset(&offsets, 0, sizeof(offsets));
    memset(&handles, 0, sizeof(handles));
    memset(&pitches, 0, sizeof(pitches));
    memset(&modifiers, 0, sizeof(modifiers));

    if(mode == HAL_DISPLAY_LAYER_MODE_MB32_NV12){
        offsets[0] = 0;
        handles[0] = creq.handle;
        pitches[0] = creq.pitch;
        modifiers[0] = DRM_FORMAT_MOD_ALLWINNER_TILED;

        offsets[1] = creq.pitch * height;
        handles[1] = creq.handle;
        pitches[1] = creq.pitch;
        modifiers[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;
    }
    else{
        offsets[0] = 0;
        handles[0] = creq.handle;
        pitches[0] = creq.pitch;
        modifiers[0] = 0;
    }

    if(mode == HAL_DISPLAY_LAYER_MODE_MB32_NV12){
        ret = drmModeAddFB2WithModifiers(fd, width, height, DRM_FORMAT_NV12, handles,
                                     pitches, offsets, modifiers, &bo->fb_id,
                                     DRM_MODE_FB_MODIFIERS);
    }
    else if(mode == HAL_DISPLAY_LAYER_MODE_RGB565){
        ret = drmModeAddFB2(fd, width, height, DRM_FORMAT_RGB565, handles, pitches, offsets,&bo->fb_id, 0);
    }
    else if(mode == HAL_DISPLAY_LAYER_MODE_ARGB1555){
        ret = drmModeAddFB2(fd, width, height, DRM_FORMAT_ARGB1555, handles, pitches, offsets,&bo->fb_id, 0);
    }
    else if(mode == HAL_DISPLAY_LAYER_MODE_ARGB8888){
        ret = drmModeAddFB2(fd, width, height, DRM_FORMAT_ARGB8888, handles, pitches, offsets,&bo->fb_id, 0);
    }

    if (ret) {
      log_error("drmModeAddFB2 return err %d", ret);
      return -1;
    }

    /* prepare buffer for memory mapping */
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    if (ret) {
      log_error("1st cannot map dumb buffer (%d): %m\n", errno);
      return -1;
    }
    /* perform actual memory mapping */
    bo->vaddr = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);

    if (bo->vaddr == MAP_FAILED) {
        log_error("2nd cannot mmap dumb buffer (%d): %m\n", errno);
      return -1;
    }

    bo->handle = creq.handle;
    bo->pitch = creq.pitch;
    bo->size = creq.size;

    return 0;
}


int hal_display_init_layer_ex(hal_display_t *hal_display,int layer_id,int width,int height,hal_display_layer_mode_t mode,int free_queue_depth){

    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    int ret;

    ret = spsc_bq_init(&layer->display_queue, 16);
    if(ret < 0){
        log_error("failed to initialize display queue");
        return -1;
    }
    // free_queue 深度须 ≥ 该层全部在飞 item 数：push 是阻塞语义，容量不足会让
    // 显示线程卡在 drain 中途(而生产侧正等它回收槽位 = 死锁)。
    ret = spsc_bq_init(&layer->free_queue, (size_t)free_queue_depth);
    if(ret < 0){
        log_error("failed to initialize free queue");
        return -1;
    }

    layer->mode = mode;
    layer->used = true;
    layer->width = width;
    layer->height = height;

    layer->curr_item = NULL;
    layer->needs_full_mount = false;

    return 0;
}

int hal_display_init_layer(hal_display_t *hal_display,int layer_id,int width,int height,hal_display_layer_mode_t mode){
    return hal_display_init_layer_ex(hal_display, layer_id, width, height, mode,
                                     HAL_DISPLAY_FREE_QUEUE_DEPTH);
}

int hal_display_destroy_layer(hal_display_t *hal_display,int layer_id){
    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    if(!layer->used){
        return 0;
    }
    spsc_bq_destroy(&layer->display_queue);
    spsc_bq_destroy(&layer->free_queue);
    layer->used = false;
    return 0;
}

int hal_display_allocate_buffer_sized(hal_display_t *hal_display,int layer_id,int width,int height,hal_buffer_t *buf){
    int ret;
    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    buf->width = width;
    buf->height = height;
    ret = hal_display_create_buffer_object(hal_display->fd, buf, width, height, layer->mode);
    if(ret < 0){
        log_error("failed to allocate buffer");
        return -1;
    }
    return 0;
}

int hal_display_allocate_buffer(hal_display_t *hal_display,int layer_id,hal_buffer_t *buf){
    hal_display_layer_t* layer = &hal_display->layer[layer_id];
    return hal_display_allocate_buffer_sized(hal_display, layer_id, layer->width, layer->height, buf);
}

int hal_display_free_buffer(hal_display_t *hal_display,int layer_id,hal_buffer_t *buf){
    struct drm_mode_destroy_dumb destroy;
    (void)layer_id;

    memset(&destroy, 0, sizeof(struct drm_mode_destroy_dumb));

    drmModeRmFB(hal_display->fd, buf->fb_id);
    munmap(buf->vaddr, buf->size);

    destroy.handle = buf->handle;
    drmIoctl(hal_display->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);

    return 0;
}

int hal_display_import_dmabuf_fb(hal_display_t *hal_display,int dmabuf_fd,int width,int height,int pitch,int uv_offset,uint32_t *fb_id,uint32_t *gem_handle){
    uint32_t handle = 0;
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t modifiers[4] = {0};
    int ret;

    ret = drmPrimeFDToHandle(hal_display->fd, dmabuf_fd, &handle);
    if(ret < 0){
        log_error("drmPrimeFDToHandle failed %s(%d)", strerror(errno), errno);
        return -1;
    }

    handles[0] = handles[1] = handle;
    pitches[0] = pitches[1] = pitch;
    offsets[0] = 0;
    offsets[1] = uv_offset;
    modifiers[0] = modifiers[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;

    ret = drmModeAddFB2WithModifiers(hal_display->fd, width, height,
                                     DRM_FORMAT_NV12, handles, pitches,
                                     offsets, modifiers, fb_id,
                                     DRM_MODE_FB_MODIFIERS);
    if(ret < 0){
        log_error("import dmabuf AddFB2 failed %s(%d)", strerror(errno), errno);
        struct drm_gem_close gc = { .handle = handle };
        drmIoctl(hal_display->fd, DRM_IOCTL_GEM_CLOSE, &gc);
        return -1;
    }
    if(gem_handle)
        *gem_handle = handle;
    return 0;
}

int hal_display_rm_fb(hal_display_t *hal_display,uint32_t fb_id,uint32_t gem_handle){
    int ret = drmModeRmFB(hal_display->fd, fb_id);
    // prime 导入的 GEM handle 是独立引用，不 close 会一直 pin 住 dmabuf
    // 背后的 CMA——REQBUFS(0) 也还不回去，反复 play/stop 几次就把 CMA 吃光
    if(gem_handle){
        struct drm_gem_close gc = { .handle = gem_handle };
        drmIoctl(hal_display->fd, DRM_IOCTL_GEM_CLOSE, &gc);
    }
    return ret;
}

int hal_display_set_layer_geometry(hal_display_t *hal_display,int layer_id,int x,int y,int src_w,int src_h,int dst_w,int dst_h){
    hal_display_layer_t* layer = &hal_display->layer[layer_id];

    layer->geo_x = (int16_t)x;
    layer->geo_y = (int16_t)y;
    layer->geo_src_w = src_w;
    layer->geo_src_h = src_h;
    layer->geo_dst_w = dst_w;
    layer->geo_dst_h = dst_h;
    layer->needs_full_mount = true;
    return 0;
}

int hal_display_disable_layer_sync(hal_display_t *hal_display,int layer_id){
    hal_display_plane_prop_ids_t *p = &hal_display->plane_props[layer_id];
    uint32_t plane_id = hal_display->plane_ids[layer_id];
    drmModeAtomicReq *req;
    int ret;

    req = drmModeAtomicAlloc();
    if(!req)
        return -1;
    drmModeAtomicAddProperty(req, plane_id, p->crtc_id, 0);
    drmModeAtomicAddProperty(req, plane_id, p->fb_id, 0);

    pthread_mutex_lock(&hal_display->commit_mutex);
    ret = drmModeAtomicCommit(hal_display->fd, req, 0, NULL);
    pthread_mutex_unlock(&hal_display->commit_mutex);
    drmModeAtomicFree(req);

    if(ret < 0)
        log_error("disable plane commit err %s(%d)", strerror(errno), errno);
    else
        hal_display->layer[layer_id].needs_full_mount = true;
    return ret;
}


// 通用挂载:src 矩形(0,0,src_w,src_h)从 buf 左上角裁,dst 矩形(x,y,dst_w,dst_h)是屏幕显示区。
//   src==dst        -> 1:1
//   src<dst / src>dst -> DEFE frontend 硬件缩放(仅 MB32 NV12 video 层;DEBE 无 scaler)
//   src_w<buf->width -> 裁掉右侧对齐 padding
// 三者可组合:如 src=(360,720) dst=(720,H) 即"先裁左 360 再放大到 720"。
// 同步 atomic commit(首挂即启用 plane;CRTC 开机已 active,无需 ALLOW_MODESET)
int hal_display_mount_layer_rect(hal_display_t *hal_display,int layer_id,int x,int y,hal_buffer_t *buf,
                                 int src_w,int src_h,int dst_w,int dst_h){
    hal_display_plane_prop_ids_t *p = &hal_display->plane_props[layer_id];
    uint32_t plane_id = hal_display->plane_ids[layer_id];
    drmModeAtomicReq *req;
    int ret;

    req = drmModeAtomicAlloc();
    if(!req){
        log_error("drmModeAtomicAlloc failed");
        return -1;
    }

    drmModeAtomicAddProperty(req, plane_id, p->crtc_id, hal_display->crtc_id);
    drmModeAtomicAddProperty(req, plane_id, p->fb_id, buf->fb_id);
    drmModeAtomicAddProperty(req, plane_id, p->src_x, 0);
    drmModeAtomicAddProperty(req, plane_id, p->src_y, 0);
    drmModeAtomicAddProperty(req, plane_id, p->src_w, (uint64_t)src_w << 16);
    drmModeAtomicAddProperty(req, plane_id, p->src_h, (uint64_t)src_h << 16);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_x, (uint64_t)(int64_t)x);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_y, (uint64_t)(int64_t)y);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_w, dst_w);
    drmModeAtomicAddProperty(req, plane_id, p->crtc_h, dst_h);

    // 显式钉死 z-order = layer_id：UI(2) 在最上，overlay(1) 居中，video(0) 最底。
    // sun4i DEBE 两级合成——先按 alpha 把各层分到 2 个 pipe(遇带 alpha 的层就抬
    // 到高 pipe)，再做 pipe 间混合。三层里只有 overlay 是 ARGB 带 alpha。zpos 正确
    // (video<overlay<UI)时 pipe0={video},pipe1={overlay,UI},UI 盖 overlay；若 UI 的
    // 归一化 zpos 掉到 overlay 之下，overlay 会独占高 pipe 反盖 UI。不设 zpos 就全
    // 靠驱动 reset 默认值，是这次 overlay 盖 UI 的根因。
    if(p->zpos)
        drmModeAtomicAddProperty(req, plane_id, p->zpos, (uint64_t)layer_id);

    pthread_mutex_lock(&hal_display->commit_mutex);
    ret = drmModeAtomicCommit(hal_display->fd, req, 0, NULL);
    pthread_mutex_unlock(&hal_display->commit_mutex);
    drmModeAtomicFree(req);

    if (ret < 0)
        log_error("mount atomic commit err %s(%d)", strerror(errno), errno);
    return ret;
}

int hal_display_mount_layer(hal_display_t *hal_display,int layer_id,int x,int y,hal_buffer_t *buf){
    return hal_display_mount_layer_rect(hal_display, layer_id, x, y, buf, buf->width, buf->height, buf->width, buf->height);
}
