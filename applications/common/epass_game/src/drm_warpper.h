#ifndef EPASS_GAME_DRM_WARPPER_H
#define EPASS_GAME_DRM_WARPPER_H

#include "spsc_queue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_WARPPER_LAYER_VIDEO 0
#define DRM_WARPPER_LAYER_OVERLAY 1

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
    bool used;
    spsc_bq_t display_queue, free_queue;
    drm_warpper_layer_mode_t mode;
    int width, height;
    drm_warpper_queue_item_t *curr_item;
} drm_warpper_layer_t;

typedef struct {
    uint32_t fb_id, crtc_id, src_x, src_y, src_w, src_h;
    uint32_t crtc_x, crtc_y, crtc_w, crtc_h, alpha;
} drm_warpper_plane_props_t;

typedef struct {
    int fd;
    drmModeConnector *conn;
    drmModeRes *res;
    drmModePlaneRes *plane_res;
    uint32_t crtc_id, conn_id;
    drm_warpper_layer_t layer[4];
    uint32_t plane_ids[4];
    drm_warpper_plane_props_t plane_props[4];
    pthread_mutex_t commit_mutex;
    pthread_t display_thread;
    atomic_int thread_running;
    bool mutex_ready, thread_started;
} drm_warpper_t;

int drm_warpper_init(drm_warpper_t *drm);
int drm_warpper_stop(drm_warpper_t *drm);
int drm_warpper_destroy(drm_warpper_t *drm);
int drm_warpper_init_layer(drm_warpper_t *drm, int layer_id, int width,
                           int height, drm_warpper_layer_mode_t mode);
int drm_warpper_allocate_buffer(drm_warpper_t *drm, int layer_id,
                                buffer_object_t *buf);
int drm_warpper_free_buffer(drm_warpper_t *drm, int layer_id,
                            buffer_object_t *buf);
int drm_warpper_mount_layer(drm_warpper_t *drm, int layer_id, int x, int y,
                            buffer_object_t *buf);
int drm_warpper_disable_layer_sync(drm_warpper_t *drm, int layer_id);
int drm_warpper_enqueue_display_item(drm_warpper_t *drm, int layer_id,
                                     drm_warpper_queue_item_t *item);
int drm_warpper_dequeue_free_item(drm_warpper_t *drm, int layer_id,
                                  drm_warpper_queue_item_t **item);

#endif
