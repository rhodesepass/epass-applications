#define _POSIX_C_SOURCE 200809L

#include "drm_warpper.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

static uint32_t property_id(int fd, uint32_t object, uint32_t type, const char *name)
{
    drmModeObjectProperties *properties = drmModeObjectGetProperties(fd, object, type);
    uint32_t found = 0;
    if(!properties) return 0;
    for(uint32_t i = 0; i < properties->count_props && !found; i++) {
        drmModePropertyRes *property = drmModeGetProperty(fd, properties->props[i]);
        if(property && strcmp(property->name, name) == 0) found = property->prop_id;
        drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    return found;
}

/* alpha 属性是 0..0xFFFF; 255*0x101 = 0xFFFF 正好回到不透明 */
static inline uint64_t alpha_to_prop(uint8_t alpha)
{
    return (uint64_t)alpha * 0x101;
}

static int discover_plane(drm_warpper_t *dw, int layer_id)
{
    uint32_t id = dw->plane_ids[layer_id];
    plane_prop_ids_t *p = &dw->props[layer_id];
    p->fb_id = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p->crtc_id = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p->src_x = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p->src_y = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p->src_w = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p->src_h = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p->crtc_x = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p->crtc_y = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p->crtc_w = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p->crtc_h = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    p->alpha = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "alpha");
    p->zpos = property_id(dw->fd, id, DRM_MODE_OBJECT_PLANE, "zpos");
    return p->fb_id && p->crtc_id && p->src_w && p->src_h &&
           p->crtc_w && p->crtc_h ? 0 : -1;
}

bool drm_warpper_layer_can_fade(const drm_warpper_t *dw, int layer_id)
{
    return layer_id >= 0 && layer_id < DRM_WARPPER_LAYER_COUNT &&
           dw->plane_ids[layer_id] && dw->props[layer_id].alpha;
}

static int ensure_modeset(drm_warpper_t *dw)
{
    drmModeCrtc *crtc = drmModeGetCrtc(dw->fd, dw->crtc_id);
    bool active = crtc && crtc->mode_valid;
    drmModeFreeCrtc(crtc);
    if(active) return 0;
    uint32_t connector_crtc = property_id(dw->fd, dw->conn_id,
                                           DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    uint32_t mode_id = property_id(dw->fd, dw->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    uint32_t active_id = property_id(dw->fd, dw->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t blob = 0;
    if(!connector_crtc || !mode_id || !active_id ||
       drmModeCreatePropertyBlob(dw->fd, &dw->conn->modes[0],
                                 sizeof(dw->conn->modes[0]), &blob) < 0) return -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) { drmModeDestroyPropertyBlob(dw->fd, blob); return -1; }
    drmModeAtomicAddProperty(req, dw->conn_id, connector_crtc, dw->crtc_id);
    drmModeAtomicAddProperty(req, dw->crtc_id, mode_id, blob);
    drmModeAtomicAddProperty(req, dw->crtc_id, active_id, 1);
    int rc = drmModeAtomicCommit(dw->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    drmModeDestroyPropertyBlob(dw->fd, blob);
    return rc;
}

int drm_warpper_init(drm_warpper_t *dw)
{
    memset(dw, 0, sizeof(*dw));
    dw->fd = -1;
    dw->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if(dw->fd < 0) return -1;
    const struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 100000000L};
    for(int retry = 0; drmSetMaster(dw->fd) != 0; retry++) {
        if(retry == 20) goto fail;
        nanosleep(&retry_delay, NULL);
    }
    if(drmSetClientCap(dw->fd, DRM_CLIENT_CAP_ATOMIC, 1) ||
       drmSetClientCap(dw->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) goto fail;
    dw->res = drmModeGetResources(dw->fd);
    if(!dw->res || !dw->res->count_crtcs) goto fail;
    dw->crtc_id = dw->res->crtcs[0];
    for(int i = 0; i < dw->res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(dw->fd, dw->res->connectors[i]);
        if(conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            dw->conn = conn; dw->conn_id = conn->connector_id; break;
        }
        drmModeFreeConnector(conn);
    }
    if(!dw->conn) goto fail;
    dw->screen_width = dw->conn->modes[0].hdisplay;
    dw->screen_height = dw->conn->modes[0].vdisplay;
    dw->plane_res = drmModeGetPlaneResources(dw->fd);
    if(!dw->plane_res || !dw->plane_res->count_planes) goto fail;
    uint32_t index = dw->plane_res->count_planes > 2 ? 2 : dw->plane_res->count_planes - 1;
    dw->plane_ids[DRM_WARPPER_LAYER_UI] = dw->plane_res->planes[index];
    if(discover_plane(dw, DRM_WARPPER_LAYER_UI) || ensure_modeset(dw)) goto fail;
    /* overlay 是锦上添花: 拿不到更高的 plane 或它没有 alpha 属性就退化成无过渡动画 */
    if(index + 1 < dw->plane_res->count_planes) {
        dw->plane_ids[DRM_WARPPER_LAYER_OVERLAY] = dw->plane_res->planes[index + 1];
        if(discover_plane(dw, DRM_WARPPER_LAYER_OVERLAY))
            dw->plane_ids[DRM_WARPPER_LAYER_OVERLAY] = 0;
    }
    return 0;
fail:
    drm_warpper_destroy(dw);
    return -1;
}

int drm_warpper_init_layer(drm_warpper_t *dw, int layer_id, int width, int height,
                           drm_warpper_layer_mode_t mode)
{
    (void)layer_id;
    if(mode != DRM_WARPPER_LAYER_MODE_RGB565) return -1;
    dw->layer_width = width;
    dw->layer_height = height;
    return 0;
}

int drm_warpper_allocate_buffer(drm_warpper_t *dw, int layer_id, buffer_object_t *buf)
{
    (void)layer_id;
    struct drm_mode_create_dumb create = {0};
    struct drm_mode_map_dumb map = {0};
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    create.width = (uint32_t)dw->layer_width;
    create.height = (uint32_t)dw->layer_height;
    create.bpp = 16;
    if(drmIoctl(dw->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) return -1;
    handles[0] = create.handle; pitches[0] = create.pitch;
    if(drmModeAddFB2(dw->fd, create.width, create.height, DRM_FORMAT_RGB565,
                     handles, pitches, offsets, &buf->fb_id, 0) < 0) goto destroy;
    map.handle = create.handle;
    if(drmIoctl(dw->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) goto remove_fb;
    buf->vaddr = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, dw->fd, map.offset);
    if(buf->vaddr == MAP_FAILED) { buf->vaddr = NULL; goto remove_fb; }
    buf->width = create.width; buf->height = create.height;
    buf->pitch = create.pitch; buf->handle = create.handle; buf->size = create.size;
    memset(buf->vaddr, 0, buf->size);
    return 0;
remove_fb:
    drmModeRmFB(dw->fd, buf->fb_id);
destroy: {
    struct drm_mode_destroy_dumb destroy_dumb = {.handle = create.handle};
    drmIoctl(dw->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
    return -1;
}}

int drm_warpper_mount_layer_alpha(drm_warpper_t *dw, int layer_id, int x, int y,
                                  buffer_object_t *buf, uint8_t alpha)
{
    if(layer_id < 0 || layer_id >= DRM_WARPPER_LAYER_COUNT) return -1;
    uint32_t id = dw->plane_ids[layer_id];
    if(!id) return -1;
    plane_prop_ids_t *p = &dw->props[layer_id];
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) return -1;
    drmModeAtomicAddProperty(req, id, p->crtc_id, dw->crtc_id);
    drmModeAtomicAddProperty(req, id, p->fb_id, buf->fb_id);
    drmModeAtomicAddProperty(req, id, p->src_x, 0);
    drmModeAtomicAddProperty(req, id, p->src_y, 0);
    drmModeAtomicAddProperty(req, id, p->src_w, (uint64_t)buf->width << 16);
    drmModeAtomicAddProperty(req, id, p->src_h, (uint64_t)buf->height << 16);
    drmModeAtomicAddProperty(req, id, p->crtc_x, (uint64_t)(int64_t)x);
    drmModeAtomicAddProperty(req, id, p->crtc_y, (uint64_t)(int64_t)y);
    drmModeAtomicAddProperty(req, id, p->crtc_w, buf->width);
    drmModeAtomicAddProperty(req, id, p->crtc_h, buf->height);
    /* zpos 默认全是 0, 那样层序就取决于 normalize 时按 plane id 排序 —— 别赌, 显式写死。
     * alpha 和 fb 同一次 commit 提交, 免得 overlay 挂上的瞬间以旧 alpha 闪一帧 */
    if(p->zpos) drmModeAtomicAddProperty(req, id, p->zpos, (uint64_t)layer_id);
    if(p->alpha) drmModeAtomicAddProperty(req, id, p->alpha, alpha_to_prop(alpha));
    int rc = drmModeAtomicCommit(dw->fd, req, 0, NULL);
    drmModeAtomicFree(req);
    if(rc == 0) dw->mounted[layer_id] = buf;
    return rc;
}

int drm_warpper_mount_layer(drm_warpper_t *dw, int layer_id, int x, int y,
                            buffer_object_t *buf)
{
    return drm_warpper_mount_layer_alpha(dw, layer_id, x, y, buf, 255);
}

int drm_warpper_set_layer_alpha(drm_warpper_t *dw, int layer_id, uint8_t alpha)
{
    if(!drm_warpper_layer_can_fade(dw, layer_id)) return -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) return -1;
    drmModeAtomicAddProperty(req, dw->plane_ids[layer_id],
                             dw->props[layer_id].alpha, alpha_to_prop(alpha));
    int rc = drmModeAtomicCommit(dw->fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return rc;
}

int drm_warpper_unmount_layer(drm_warpper_t *dw, int layer_id)
{
    if(layer_id < 0 || layer_id >= DRM_WARPPER_LAYER_COUNT) return -1;
    uint32_t id = dw->plane_ids[layer_id];
    if(!id || !dw->props[layer_id].fb_id) return -1;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) return -1;
    drmModeAtomicAddProperty(req, id, dw->props[layer_id].crtc_id, 0);
    drmModeAtomicAddProperty(req, id, dw->props[layer_id].fb_id, 0);
    int rc = drmModeAtomicCommit(dw->fd, req, 0, NULL);
    drmModeAtomicFree(req);
    if(rc == 0) dw->mounted[layer_id] = NULL;
    return rc;
}

int drm_warpper_free_buffer(drm_warpper_t *dw, buffer_object_t *buf)
{
    if(dw->fd < 0 || !buf) return -1;
    if(buf->fb_id) drmModeRmFB(dw->fd, buf->fb_id);
    if(buf->vaddr) munmap(buf->vaddr, buf->size);
    if(buf->handle) {
        struct drm_mode_destroy_dumb destroy = {.handle = buf->handle};
        drmIoctl(dw->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    for(int i = 0; i < DRM_WARPPER_LAYER_COUNT; i++)
        if(dw->mounted[i] == buf) dw->mounted[i] = NULL;
    memset(buf, 0, sizeof(*buf));
    return 0;
}

int drm_warpper_destroy(drm_warpper_t *dw)
{
    for(int i = 0; dw->fd >= 0 && i < DRM_WARPPER_LAYER_COUNT; i++)
        drm_warpper_unmount_layer(dw, i);
    for(int i = 0; i < DRM_WARPPER_LAYER_COUNT; i++)
        if(dw->mounted[i]) drm_warpper_free_buffer(dw, dw->mounted[i]);
    drmModeFreePlaneResources(dw->plane_res);
    drmModeFreeConnector(dw->conn);
    drmModeFreeResources(dw->res);
    if(dw->fd >= 0) { drmDropMaster(dw->fd); close(dw->fd); }
    dw->fd = -1;
    return 0;
}
