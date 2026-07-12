#include "drm_warpper.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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

static int discover_plane(drm_warpper_t *dw)
{
    plane_prop_ids_t *p = &dw->props;
    p->fb_id = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p->crtc_id = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p->src_x = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p->src_y = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p->src_w = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p->src_h = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p->crtc_x = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p->crtc_y = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p->crtc_w = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p->crtc_h = property_id(dw->fd, dw->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    return p->fb_id && p->crtc_id && p->src_w && p->src_h &&
           p->crtc_w && p->crtc_h ? 0 : -1;
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
    for(int retry = 0; drmSetMaster(dw->fd) != 0; retry++) {
        if(retry == 20) goto fail;
        usleep(100000);
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
    dw->plane_id = dw->plane_res->planes[index];
    if(discover_plane(dw) || ensure_modeset(dw)) goto fail;
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

int drm_warpper_mount_layer(drm_warpper_t *dw, int layer_id, int x, int y,
                            buffer_object_t *buf)
{
    (void)layer_id;
    plane_prop_ids_t *p = &dw->props;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if(!req) return -1;
    drmModeAtomicAddProperty(req, dw->plane_id, p->crtc_id, dw->crtc_id);
    drmModeAtomicAddProperty(req, dw->plane_id, p->fb_id, buf->fb_id);
    drmModeAtomicAddProperty(req, dw->plane_id, p->src_x, 0);
    drmModeAtomicAddProperty(req, dw->plane_id, p->src_y, 0);
    drmModeAtomicAddProperty(req, dw->plane_id, p->src_w, (uint64_t)buf->width << 16);
    drmModeAtomicAddProperty(req, dw->plane_id, p->src_h, (uint64_t)buf->height << 16);
    drmModeAtomicAddProperty(req, dw->plane_id, p->crtc_x, (uint64_t)(int64_t)x);
    drmModeAtomicAddProperty(req, dw->plane_id, p->crtc_y, (uint64_t)(int64_t)y);
    drmModeAtomicAddProperty(req, dw->plane_id, p->crtc_w, buf->width);
    drmModeAtomicAddProperty(req, dw->plane_id, p->crtc_h, buf->height);
    int rc = drmModeAtomicCommit(dw->fd, req, 0, NULL);
    drmModeAtomicFree(req);
    if(rc == 0) dw->mounted = buf;
    return rc;
}

int drm_warpper_destroy(drm_warpper_t *dw)
{
    if(dw->fd >= 0 && dw->mounted) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if(req) {
            drmModeAtomicAddProperty(req, dw->plane_id, dw->props.crtc_id, 0);
            drmModeAtomicAddProperty(req, dw->plane_id, dw->props.fb_id, 0);
            drmModeAtomicCommit(dw->fd, req, 0, NULL);
            drmModeAtomicFree(req);
        }
        drmModeRmFB(dw->fd, dw->mounted->fb_id);
        munmap(dw->mounted->vaddr, dw->mounted->size);
        struct drm_mode_destroy_dumb destroy = {.handle = dw->mounted->handle};
        drmIoctl(dw->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    drmModeFreePlaneResources(dw->plane_res);
    drmModeFreeConnector(dw->conn);
    drmModeFreeResources(dw->res);
    if(dw->fd >= 0) { drmDropMaster(dw->fd); close(dw->fd); }
    dw->fd = -1;
    return 0;
}
