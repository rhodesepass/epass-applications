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

/* 逻辑层。UI 沿用原先那个 plane, overlay 取更高一档并显式抬 zpos 盖在 UI 上。
 * sun4i 只在 pipe1 最低位允许可变 alpha, 且默认最多 1 个 alpha 平面; suniv 的
 * quirk supports_lowest_plane_alpha=true 把上限放到 2 并撤掉最低位限制, 所以
 * 这里 overlay 变 alpha 是合法的 (见 sun4i_backend_atomic_check)。 */
typedef enum {
    DRM_WARPPER_LAYER_UI = 0,
    DRM_WARPPER_LAYER_OVERLAY,
    DRM_WARPPER_LAYER_COUNT
} drm_warpper_layer_t;

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
    uint32_t alpha, zpos; /* 可能为 0: 驱动没暴露该属性 */
} plane_prop_ids_t;

typedef struct {
    int fd;
    drmModeRes *res;
    drmModeConnector *conn;
    drmModePlaneRes *plane_res;
    uint32_t conn_id, crtc_id;
    uint32_t plane_ids[DRM_WARPPER_LAYER_COUNT];
    plane_prop_ids_t props[DRM_WARPPER_LAYER_COUNT];
    buffer_object_t *mounted[DRM_WARPPER_LAYER_COUNT];
    int screen_width, screen_height;
    int layer_width, layer_height;
} drm_warpper_t;

int drm_warpper_init(drm_warpper_t *dw);
int drm_warpper_init_layer(drm_warpper_t *dw, int layer_id, int width, int height,
                           drm_warpper_layer_mode_t mode);
int drm_warpper_allocate_buffer(drm_warpper_t *dw, int layer_id, buffer_object_t *buf);
int drm_warpper_mount_layer(drm_warpper_t *dw, int layer_id, int x, int y,
                            buffer_object_t *buf);
/* alpha 0..255, 内部映射到 DRM 的 0..0xffff */
int drm_warpper_mount_layer_alpha(drm_warpper_t *dw, int layer_id, int x, int y,
                                  buffer_object_t *buf, uint8_t alpha);
int drm_warpper_set_layer_alpha(drm_warpper_t *dw, int layer_id, uint8_t alpha);
int drm_warpper_unmount_layer(drm_warpper_t *dw, int layer_id);
/* 该层是否拿到了 plane 且支持 alpha (overlay 动画的前提) */
bool drm_warpper_layer_can_fade(const drm_warpper_t *dw, int layer_id);
int drm_warpper_free_buffer(drm_warpper_t *dw, buffer_object_t *buf);
int drm_warpper_destroy(drm_warpper_t *dw);
