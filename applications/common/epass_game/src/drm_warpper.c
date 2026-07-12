#include "drm_warpper.h"
#include "log.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint32_t find_prop(int fd, uint32_t object, uint32_t type,
                          const char *name)
{
    drmModeObjectProperties *props;
    uint32_t result = 0;
    props = drmModeObjectGetProperties(fd, object, type);
    if(!props) return 0;
    for(uint32_t i = 0; i < props->count_props && !result; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if(prop) {
            if(strcmp(prop->name, name) == 0) result = prop->prop_id;
            drmModeFreeProperty(prop);
        }
    }
    drmModeFreeObjectProperties(props);
    return result;
}

static int discover_plane(drm_warpper_t *drm, int layer_id)
{
    drm_warpper_plane_props_t *p = &drm->plane_props[layer_id];
    uint32_t id = drm->plane_ids[layer_id];
    memset(p, 0, sizeof(*p));
    p->fb_id = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p->crtc_id = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p->src_x = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p->src_y = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p->src_w = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p->src_h = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p->crtc_x = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p->crtc_y = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p->crtc_w = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p->crtc_h = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    p->alpha = find_prop(drm->fd, id, DRM_MODE_OBJECT_PLANE, "alpha");
    return p->fb_id && p->crtc_id && p->src_x && p->src_y &&
           p->src_w && p->src_h && p->crtc_x && p->crtc_y &&
           p->crtc_w && p->crtc_h ? 0 : -1;
}

static bool supported_mode(const drmModeModeInfo *mode)
{
    return (mode->hdisplay == 360 && mode->vdisplay == 640) ||
           (mode->hdisplay == 480 && mode->vdisplay == 854) ||
           (mode->hdisplay == 720 && mode->vdisplay == 1280);
}

static int choose_connector(drm_warpper_t *drm)
{
    for(int i = 0; i < drm->res->count_connectors; i++) {
        drmModeConnector *conn =
            drmModeGetConnector(drm->fd, drm->res->connectors[i]);
        if(!conn) continue;
        if(conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            int selected = -1;
            for(int m = 0; m < conn->count_modes; m++) {
                if(supported_mode(&conn->modes[m])) {
                    selected = m;
                    if(conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) break;
                }
            }
            if(selected >= 0) {
                if(selected != 0) {
                    drmModeModeInfo tmp = conn->modes[0];
                    conn->modes[0] = conn->modes[selected];
                    conn->modes[selected] = tmp;
                }
                drm->conn = conn;
                drm->conn_id = conn->connector_id;
                return 0;
            }
        }
        drmModeFreeConnector(conn);
    }
    log_error("no connected 360x640, 480x854 or 720x1280 connector");
    return -1;
}

static int choose_crtc(drm_warpper_t *drm)
{
    if(drm->conn->encoder_id) {
        drmModeEncoder *encoder =
            drmModeGetEncoder(drm->fd, drm->conn->encoder_id);
        if(encoder) {
            if(encoder->crtc_id) drm->crtc_id = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
        }
    }
    if(!drm->crtc_id && drm->res->count_crtcs)
        drm->crtc_id = drm->res->crtcs[0];
    return drm->crtc_id ? 0 : -1;
}

static int ensure_crtc(drm_warpper_t *drm)
{
    drmModeCrtc *crtc = drmModeGetCrtc(drm->fd, drm->crtc_id);
    bool active = crtc && crtc->mode_valid;
    if(crtc) drmModeFreeCrtc(crtc);
    if(active) return 0;

    uint32_t conn_crtc = find_prop(drm->fd, drm->conn_id,
                                    DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    uint32_t mode_id = find_prop(drm->fd, drm->crtc_id,
                                  DRM_MODE_OBJECT_CRTC, "MODE_ID");
    uint32_t active_id = find_prop(drm->fd, drm->crtc_id,
                                    DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t blob = 0;
    if(!conn_crtc || !mode_id || !active_id ||
       drmModeCreatePropertyBlob(drm->fd, &drm->conn->modes[0],
                                 sizeof(drm->conn->modes[0]), &blob) < 0)
        return -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) {
        drmModeDestroyPropertyBlob(drm->fd, blob);
        return -1;
    }
    drmModeAtomicAddProperty(req, drm->conn_id, conn_crtc, drm->crtc_id);
    drmModeAtomicAddProperty(req, drm->crtc_id, mode_id, blob);
    drmModeAtomicAddProperty(req, drm->crtc_id, active_id, 1);
    int ret = drmModeAtomicCommit(drm->fd, req,
                                  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(drm->fd, blob);
    return ret;
}

static void wait_vblank(drm_warpper_t *drm)
{
    drmVBlank blank;
    memset(&blank, 0, sizeof(blank));
    blank.request.type = DRM_VBLANK_RELATIVE;
    blank.request.sequence = 1;
    if(drmWaitVBlank(drm->fd, &blank) < 0) usleep(16000);
}

static void *display_main(void *argument)
{
    drm_warpper_t *drm = argument;
    while(atomic_load(&drm->thread_running)) {
        drmModeAtomicReq *req = NULL;
        drm_warpper_queue_item_t *drained[4][16] = {{0}};
        int drained_count[4] = {0};
        drm_warpper_queue_item_t *item;
        bool work = false;

        for(int i = 0; i < 4; i++) {
            drm_warpper_layer_t *layer = &drm->layer[i];
            if(!layer->used) continue;
            while(spsc_bq_try_pop(&layer->display_queue,
                                  (void **)&item) == 0) {
                if(item->type != DRM_WARPPER_ITEM_FLIP_FB) {
                    if(item->on_heap) free(item);
                    continue;
                }
                if(drained_count[i] < 16)
                    drained[i][drained_count[i]++] = item;
            }
            if(drained_count[i]) {
                if(!req) req = drmModeAtomicAlloc();
                if(req) {
                    item = drained[i][drained_count[i] - 1];
                    drmModeAtomicAddProperty(req, drm->plane_ids[i],
                        drm->plane_props[i].fb_id, item->fb_id);
                    work = true;
                }
            }
        }

        int ret = -1;
        if(work && req) {
            pthread_mutex_lock(&drm->commit_mutex);
            ret = drmModeAtomicCommit(drm->fd, req, 0, NULL);
            pthread_mutex_unlock(&drm->commit_mutex);
            if(ret < 0) log_error("atomic flip failed: %s", strerror(errno));
        }
        if(req) drmModeAtomicFree(req);
        for(int i = 0; i < 4; i++) {
            int count = drained_count[i];
            if(!count) continue;
            drm_warpper_layer_t *layer = &drm->layer[i];
            if(ret == 0) {
                for(int n = 0; n < count - 1; n++)
                    spsc_bq_push(&layer->free_queue, drained[i][n]);
                if(layer->curr_item)
                    spsc_bq_push(&layer->free_queue, layer->curr_item);
                layer->curr_item = drained[i][count - 1];
            } else {
                for(int n = 0; n < count; n++)
                    spsc_bq_push(&layer->free_queue, drained[i][n]);
            }
        }
        if(!work) wait_vblank(drm);
    }
    return NULL;
}

int drm_warpper_init(drm_warpper_t *drm)
{
    memset(drm, 0, sizeof(*drm));
    drm->fd = -1;
    drm->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if(drm->fd < 0) goto fail;
    for(int tries = 0; drmSetMaster(drm->fd) != 0; tries++) {
        if(tries == 20) goto fail;
        usleep(100000);
    }
    if(drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1) ||
       drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
        goto fail;
    drm->res = drmModeGetResources(drm->fd);
    if(!drm->res || choose_connector(drm) || choose_crtc(drm)) goto fail;
    drm->plane_res = drmModeGetPlaneResources(drm->fd);
    if(!drm->plane_res || drm->plane_res->count_planes < 2) goto fail;
    for(uint32_t i = 0; i < 4 && i < drm->plane_res->count_planes; i++) {
        drm->plane_ids[i] = drm->plane_res->planes[i];
        if(discover_plane(drm, (int)i)) goto fail;
    }
    if(ensure_crtc(drm)) goto fail;
    if(pthread_mutex_init(&drm->commit_mutex, NULL)) goto fail;
    drm->mutex_ready = true;
    atomic_store(&drm->thread_running, 1);
    if(pthread_create(&drm->display_thread, NULL, display_main, drm)) goto fail;
    drm->thread_started = true;
    log_info("display mode %dx%d", drm->conn->modes[0].hdisplay,
             drm->conn->modes[0].vdisplay);
    return 0;
fail:
    log_error("DRM initialization failed: %s", strerror(errno));
    drm_warpper_destroy(drm);
    return -1;
}

int drm_warpper_stop(drm_warpper_t *drm)
{
    if(drm->thread_started) {
        atomic_store(&drm->thread_running, 0);
        pthread_join(drm->display_thread, NULL);
        drm->thread_started = false;
    }
    return 0;
}

int drm_warpper_destroy(drm_warpper_t *drm)
{
    drm_warpper_stop(drm);
    for(int i = 0; i < 4; i++) {
        if(drm->layer[i].used) {
            spsc_bq_destroy(&drm->layer[i].display_queue);
            spsc_bq_destroy(&drm->layer[i].free_queue);
            drm->layer[i].used = false;
        }
    }
    if(drm->mutex_ready) pthread_mutex_destroy(&drm->commit_mutex);
    if(drm->conn) drmModeFreeConnector(drm->conn);
    if(drm->plane_res) drmModeFreePlaneResources(drm->plane_res);
    if(drm->res) drmModeFreeResources(drm->res);
    if(drm->fd >= 0) close(drm->fd);
    memset(drm, 0, sizeof(*drm));
    drm->fd = -1;
    return 0;
}

int drm_warpper_init_layer(drm_warpper_t *drm, int id, int width,
                           int height, drm_warpper_layer_mode_t mode)
{
    if(id < 0 || id >= 4 || !drm->plane_ids[id]) return -1;
    drm_warpper_layer_t *layer = &drm->layer[id];
    if(spsc_bq_init(&layer->display_queue, 16)) return -1;
    if(spsc_bq_init(&layer->free_queue, 20)) {
        spsc_bq_destroy(&layer->display_queue);
        return -1;
    }
    layer->mode = mode;
    layer->width = width;
    layer->height = height;
    layer->used = true;
    return 0;
}

int drm_warpper_allocate_buffer(drm_warpper_t *drm, int id,
                                buffer_object_t *buf)
{
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    drm_warpper_layer_t *layer = &drm->layer[id];
    memset(buf, 0, sizeof(*buf));
    create.width = (uint32_t)layer->width;
    create.height = (uint32_t)layer->height;
    create.bpp = layer->mode == DRM_WARPPER_LAYER_MODE_RGB565 ? 16 : 32;
    if(drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) return -1;
    handles[0] = create.handle;
    pitches[0] = create.pitch;
    uint32_t format = layer->mode == DRM_WARPPER_LAYER_MODE_RGB565 ?
                      DRM_FORMAT_RGB565 : DRM_FORMAT_ARGB8888;
    if(drmModeAddFB2(drm->fd, create.width, create.height, format,
                     handles, pitches, offsets, &buf->fb_id, 0) < 0)
        goto fail_handle;
    map.handle = create.handle;
    if(drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) goto fail_fb;
    buf->vaddr = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      drm->fd, map.offset);
    if(buf->vaddr == MAP_FAILED) {
        buf->vaddr = NULL;
        goto fail_fb;
    }
    buf->width = create.width;
    buf->height = create.height;
    buf->pitch = create.pitch;
    buf->handle = create.handle;
    buf->size = (uint32_t)create.size;
    return 0;
fail_fb:
    drmModeRmFB(drm->fd, buf->fb_id);
fail_handle: {
    struct drm_mode_destroy_dumb destroy = {.handle = create.handle};
    drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    return -1;
}}

int drm_warpper_free_buffer(drm_warpper_t *drm, int id,
                            buffer_object_t *buf)
{
    (void)id;
    if(!buf->handle) return 0;
    struct drm_mode_destroy_dumb destroy = {.handle = buf->handle};
    if(buf->fb_id) drmModeRmFB(drm->fd, buf->fb_id);
    if(buf->vaddr) munmap(buf->vaddr, buf->size);
    drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    memset(buf, 0, sizeof(*buf));
    return 0;
}

int drm_warpper_mount_layer(drm_warpper_t *drm, int id, int x, int y,
                            buffer_object_t *buf)
{
    drm_warpper_plane_props_t *p = &drm->plane_props[id];
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) return -1;
    uint32_t plane = drm->plane_ids[id];
    drmModeAtomicAddProperty(req, plane, p->crtc_id, drm->crtc_id);
    drmModeAtomicAddProperty(req, plane, p->fb_id, buf->fb_id);
    drmModeAtomicAddProperty(req, plane, p->src_x, 0);
    drmModeAtomicAddProperty(req, plane, p->src_y, 0);
    drmModeAtomicAddProperty(req, plane, p->src_w, (uint64_t)buf->width << 16);
    drmModeAtomicAddProperty(req, plane, p->src_h, (uint64_t)buf->height << 16);
    drmModeAtomicAddProperty(req, plane, p->crtc_x, (uint64_t)(int64_t)x);
    drmModeAtomicAddProperty(req, plane, p->crtc_y, (uint64_t)(int64_t)y);
    drmModeAtomicAddProperty(req, plane, p->crtc_w, buf->width);
    drmModeAtomicAddProperty(req, plane, p->crtc_h, buf->height);
    if(p->alpha)
        drmModeAtomicAddProperty(req, plane, p->alpha, 0xffff);
    pthread_mutex_lock(&drm->commit_mutex);
    int ret = drmModeAtomicCommit(drm->fd, req, 0, NULL);
    pthread_mutex_unlock(&drm->commit_mutex);
    drmModeAtomicFree(req);
    return ret;
}

int drm_warpper_disable_layer_sync(drm_warpper_t *drm, int id)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) return -1;
    drmModeAtomicAddProperty(req, drm->plane_ids[id],
                             drm->plane_props[id].crtc_id, 0);
    drmModeAtomicAddProperty(req, drm->plane_ids[id],
                             drm->plane_props[id].fb_id, 0);
    pthread_mutex_lock(&drm->commit_mutex);
    int ret = drmModeAtomicCommit(drm->fd, req, 0, NULL);
    pthread_mutex_unlock(&drm->commit_mutex);
    drmModeAtomicFree(req);
    return ret;
}

int drm_warpper_enqueue_display_item(drm_warpper_t *drm, int id,
                                     drm_warpper_queue_item_t *item)
{
    return spsc_bq_push(&drm->layer[id].display_queue, item);
}

int drm_warpper_dequeue_free_item(drm_warpper_t *drm, int id,
                                  drm_warpper_queue_item_t **item)
{
    return spsc_bq_pop(&drm->layer[id].free_queue, (void **)item);
}
