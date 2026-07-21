/*
 * hal_display 的 wasm/浏览器后端: 每个 layer 一张 canvas, 叠放次序 = z-index =
 * 层号(与 epass 的 plane 索引语义一致), alpha = CSS opacity。
 * 只实现"wasm 可实现子集"(同步 mount 家族); 异步 enqueue/dequeue、dmabuf、
 * NV12 属于 epass-only, 调用即报错——视频那条路 wasm 另走。
 * 本文件只由 tools/build_wasm_lvgl.sh 用 emcc 编, 不进 CMake。
 */
#ifndef __EMSCRIPTEN__
#error "hal_display_wasm.c 只用于 emscripten 构建"
#endif

#include "hal_display.h"
#include "log.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* 与游戏后端一致: 浏览器端直接用逻辑分辨率 */
#define WASM_DISPLAY_WIDTH 360
#define WASM_DISPLAY_HEIGHT 640

/* mount 时把各种格式统一转成 canvas 要的 RGBA 字节序 */
static uint32_t *staging;

int hal_display_init(hal_display_t *hal_display)
{
    memset(hal_display, 0, sizeof(*hal_display));
    hal_display->fd = -1;
    staging = malloc((size_t)WASM_DISPLAY_WIDTH * WASM_DISPLAY_HEIGHT * 4);
    if(!staging) return -1;

    EM_ASM({
        /* 幂等: fg_ext 应用退出后可再次 callMain, DOM 结构只建一次 */
        if (!Module.halWrap) {
            var base = document.getElementById('canvas');
            /* 包一层相对定位容器, 其余 layer 的 canvas 绝对定位盖在 base 上,
             * 跟随 base 的 CSS 缩放 */
            var wrap = document.createElement('div');
            wrap.style.position = 'relative';
            wrap.style.lineHeight = '0';
            base.parentNode.insertBefore(wrap, base);
            wrap.appendChild(base);
            base.style.position = 'relative';
            base.style.zIndex = 2; /* base canvas 留给 UI 层(层号 2) */
            Module.halWrap = wrap;
            Module.halLayers = { 2: base };
        }
        Module.halLayers[2].width = $0;
        Module.halLayers[2].height = $1;
        /* 清掉上一轮运行残留的层状态 */
        for (var id in Module.halLayers) {
            var c = Module.halLayers[id];
            c.style.opacity = '';
            c.style.visibility = id == 2 ? '' : 'hidden';
        }
    }, WASM_DISPLAY_WIDTH, WASM_DISPLAY_HEIGHT);
    return 0;
}

int hal_display_stop(hal_display_t *hal_display)
{
    (void)hal_display;
    return 0;
}

int hal_display_destroy(hal_display_t *hal_display)
{
    (void)hal_display;
    free(staging);
    staging = NULL;
    /* 模拟设备退出回到黑屏; onHalExit 让外壳页(如 VFS 面板)知道可以再启动 */
    EM_ASM({
        if (Module.halLayers) {
            for (var id in Module.halLayers) {
                var c = Module.halLayers[id];
                c.style.opacity = '';
                if (id == 2) {
                    var ctx = c.getContext('2d');
                    ctx.fillStyle = '#000';
                    ctx.fillRect(0, 0, c.width, c.height);
                } else {
                    c.style.visibility = 'hidden';
                }
            }
        }
        if (Module.onHalExit) setTimeout(Module.onHalExit, 0);
    });
    return 0;
}

int hal_display_display_size(const hal_display_t *hal_display, int *width,
                             int *height)
{
    (void)hal_display;
    if(width) *width = WASM_DISPLAY_WIDTH;
    if(height) *height = WASM_DISPLAY_HEIGHT;
    return 0;
}

int hal_display_init_layer_ex(hal_display_t *hal_display, int layer_id,
                              int width, int height,
                              hal_display_layer_mode_t mode,
                              int free_queue_depth)
{
    (void)free_queue_depth;
    if(layer_id < 0 || layer_id >= 4) return -1;
    if(mode == HAL_DISPLAY_LAYER_MODE_MB32_NV12) return -1; /* epass-only */
    hal_display_layer_t *layer = &hal_display->layer[layer_id];
    layer->used = true;
    layer->mode = mode;
    layer->width = width;
    layer->height = height;

    /* layer canvas 全部做成整屏大小, mount 的 (x,y) 用 putImageData 偏移表达 */
    EM_ASM({
        if(!Module.halLayers[$0]) {
            var c = document.createElement('canvas');
            c.width = $1;
            c.height = $2;
            c.style.position = 'absolute';
            c.style.left = '0';
            c.style.top = '0';
            c.style.width = '100%';
            c.style.height = '100%';
            c.style.zIndex = $0;
            Module.halWrap.appendChild(c);
            Module.halLayers[$0] = c;
        }
    }, layer_id, WASM_DISPLAY_WIDTH, WASM_DISPLAY_HEIGHT);
    return 0;
}

int hal_display_init_layer(hal_display_t *hal_display, int layer_id, int width,
                           int height, hal_display_layer_mode_t mode)
{
    return hal_display_init_layer_ex(hal_display, layer_id, width, height,
                                     mode, 0);
}

int hal_display_destroy_layer(hal_display_t *hal_display, int layer_id)
{
    if(layer_id < 0 || layer_id >= 4) return -1;
    hal_display->layer[layer_id].used = false;
    return 0;
}

static int mode_bpp(hal_display_layer_mode_t mode)
{
    return (mode == HAL_DISPLAY_LAYER_MODE_ARGB8888) ? 4 : 2;
}

int hal_display_allocate_buffer_sized(hal_display_t *hal_display, int layer_id,
                                      int width, int height, hal_buffer_t *buf)
{
    hal_display_layer_t *layer = &hal_display->layer[layer_id];
    int bpp = mode_bpp(layer->mode);
    memset(buf, 0, sizeof(*buf));
    buf->width = (uint32_t)width;
    buf->height = (uint32_t)height;
    buf->pitch = (uint32_t)(width * bpp);
    buf->size = buf->pitch * (uint32_t)height;
    buf->vaddr = calloc(1, buf->size);
    return buf->vaddr ? 0 : -1;
}

int hal_display_allocate_buffer(hal_display_t *hal_display, int layer_id,
                                hal_buffer_t *buf)
{
    hal_display_layer_t *layer = &hal_display->layer[layer_id];
    return hal_display_allocate_buffer_sized(hal_display, layer_id,
                                             layer->width, layer->height, buf);
}

int hal_display_free_buffer(hal_display_t *hal_display, int layer_id,
                            hal_buffer_t *buf)
{
    (void)hal_display;
    (void)layer_id;
    free(buf->vaddr);
    buf->vaddr = NULL;
    return 0;
}

/* buf 内容(按层格式)转 RGBA 并画到该层 canvas 的 (x,y); 顺带取消隐藏 */
static int wasm_blit(hal_display_t *hal_display, int layer_id, int x, int y,
                     const hal_buffer_t *buf)
{
    hal_display_layer_t *layer = &hal_display->layer[layer_id];
    int w = (int)buf->width, h = (int)buf->height;
    if(!buf->vaddr || w <= 0 || h <= 0) return -1;
    if((size_t)w * h > (size_t)WASM_DISPLAY_WIDTH * WASM_DISPLAY_HEIGHT)
        return -1;

    if(layer->mode == HAL_DISPLAY_LAYER_MODE_ARGB8888) {
        const uint32_t *src = (const uint32_t *)buf->vaddr;
        int stride = (int)(buf->pitch / 4);
        for(int py = 0; py < h; py++)
            for(int px = 0; px < w; px++) {
                uint32_t p = src[py * stride + px];
                staging[py * w + px] = (p & 0xff00ff00u) |
                                       ((p & 0xff0000u) >> 16) |
                                       ((p & 0xffu) << 16);
            }
    } else { /* RGB565 / ARGB1555 都按 565 展开, 1555 目前无消费者 */
        const uint16_t *src = (const uint16_t *)buf->vaddr;
        int stride = (int)(buf->pitch / 2);
        for(int py = 0; py < h; py++)
            for(int px = 0; px < w; px++) {
                uint16_t p = src[py * stride + px];
                uint32_t r = (uint32_t)(p >> 11) << 3;
                uint32_t g = (uint32_t)((p >> 5) & 0x3f) << 2;
                uint32_t b = (uint32_t)(p & 0x1f) << 3;
                staging[py * w + px] = 0xff000000u | (b << 16) | (g << 8) | r;
            }
    }

    EM_ASM({
        var c = Module.halLayers[$0];
        c.style.visibility = '';
        var img = new ImageData(
            new Uint8ClampedArray(HEAPU8.buffer, $1, $2 * $3 * 4), $2, $3);
        c.getContext('2d').putImageData(img, $4, $5);
    }, layer_id, staging, w, h, x, y);
    return 0;
}

int hal_display_mount_layer(hal_display_t *hal_display, int layer_id, int x,
                            int y, hal_buffer_t *buf)
{
    return wasm_blit(hal_display, layer_id, x, y, buf);
}

bool hal_display_layer_can_fade(const hal_display_t *hal_display, int layer_id)
{
    (void)hal_display;
    return layer_id >= 0 && layer_id < 4;
}

int hal_display_set_layer_alpha(hal_display_t *hal_display, int layer_id,
                                uint8_t alpha)
{
    (void)hal_display;
    EM_ASM({
        var c = Module.halLayers[$0];
        if(c) c.style.opacity = $1 / 255;
    }, layer_id, alpha);
    return 0;
}

int hal_display_mount_layer_alpha(hal_display_t *hal_display, int layer_id,
                                  int x, int y, hal_buffer_t *buf,
                                  uint8_t alpha)
{
    if(hal_display_set_layer_alpha(hal_display, layer_id, alpha) < 0)
        return -1;
    return wasm_blit(hal_display, layer_id, x, y, buf);
}

int hal_display_disable_layer_sync(hal_display_t *hal_display, int layer_id)
{
    (void)hal_display;
    EM_ASM({
        var c = Module.halLayers[$0];
        if(c) c.style.visibility = 'hidden';
    }, layer_id);
    return 0;
}

/* ---- epass-only 的异步/视频路径: wasm 不提供 ---- */

int hal_display_enqueue_display_item(hal_display_t *hal_display, int layer_id,
                                     hal_display_queue_item_t *item)
{
    (void)hal_display; (void)layer_id; (void)item;
    log_error("enqueue_display_item: epass-only, not available on wasm");
    return -1;
}

int hal_display_dequeue_free_item(hal_display_t *hal_display, int layer_id,
                                  hal_display_queue_item_t **item)
{
    (void)hal_display; (void)layer_id; (void)item;
    log_error("dequeue_free_item: epass-only, not available on wasm");
    return -1;
}
