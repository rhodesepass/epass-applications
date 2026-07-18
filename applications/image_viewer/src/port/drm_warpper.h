#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xf86drmMode.h>

typedef enum {
    DRM_WARPPER_LAYER_MODE_RGB565,
    DRM_WARPPER_LAYER_MODE_ARGB8888,
    DRM_WARPPER_LAYER_MODE_MB32_NV12
} drm_warpper_layer_mode_t;

typedef enum {
    DRM_WARPPER_ITEM_FLIP_FB,
    DRM_WARPPER_ITEM_SET_COORD,
    DRM_WARPPER_ITEM_SET_ALPHA
} drm_warpper_item_type_t;

typedef struct {
    uint32_t width, height, pitch, handle, size;
    uint8_t *vaddr;
    uint32_t fb_id;
} buffer_object_t;

typedef struct {
    drm_warpper_item_type_t type;
    uint32_t fb_id;
    int16_t x, y;
    uint8_t alpha;
    void *userdata;
    bool on_heap;
} drm_warpper_queue_item_t;

typedef struct {
    uint32_t fb_id, crtc_id, src_x, src_y, src_w, src_h;
    uint32_t crtc_x, crtc_y, crtc_w, crtc_h;
} plane_prop_ids_t;

typedef struct {
    int fd;
    drmModeRes *res;
    drmModeConnector *conn;
    drmModePlaneRes *plane_res;
    uint32_t conn_id, crtc_id, plane_id;
    plane_prop_ids_t props;
    int screen_width, screen_height;
    int layer_width, layer_height;
    buffer_object_t *mounted;
} drm_warpper_t;

int drm_warpper_init(drm_warpper_t *dw);
int drm_warpper_init_layer(drm_warpper_t *dw, int layer_id, int width, int height,
                           drm_warpper_layer_mode_t mode);
int drm_warpper_allocate_buffer(drm_warpper_t *dw, int layer_id, buffer_object_t *buf);
int drm_warpper_mount_layer(drm_warpper_t *dw, int layer_id, int x, int y,
                            buffer_object_t *buf);
int drm_warpper_free_buffer(drm_warpper_t *dw, buffer_object_t *buf);
int drm_warpper_destroy(drm_warpper_t *dw);
