/* 离屏渲染教程各页到 PNG (dev 工具, 无需设备):
 *   preview <输出目录> [宽 高]     默认 360 640
 * 构建: cmake -DQUICK_START_PREVIEW=ON, 目标 quick_start_preview */
#define _POSIX_C_SOURCE 200809L

#include "ui/ui.h"

#include <lvgl.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area; (void)px; /* DIRECT 模式, 帧就在我们自己的缓冲里 */
    lv_display_flush_ready(disp);
}

static bool save_png(const char *path, const uint8_t *rgb565, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    if(!fp) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(png);
    if(setjmp(png_jmpbuf(png))) { fclose(fp); return false; }
    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    uint8_t *row = malloc((size_t)w * 3);
    for(int y = 0; y < h; y++) {
        for(int x = 0; x < w; x++) {
            uint16_t p = ((const uint16_t *)rgb565)[(size_t)y * w + x];
            row[x * 3 + 0] = (uint8_t)(((p >> 11) & 0x1f) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3f) * 255 / 63);
            row[x * 3 + 2] = (uint8_t)((p & 0x1f) * 255 / 31);
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

int main(int argc, char **argv)
{
    if(argc < 2) { fprintf(stderr, "usage: %s <outdir> [w h]\n", argv[0]); return 1; }
    const char *outdir = argv[1];
    int w = argc > 3 ? atoi(argv[2]) : 360;
    int h = argc > 3 ? atoi(argv[3]) : 640;

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(w, h);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    uint8_t *fb = malloc((size_t)w * h * 2);
    lv_display_set_buffers(disp, fb, NULL, (uint32_t)((size_t)w * h * 2),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    tutorial_platform_t plat;
    memset(&plat, 0, sizeof(plat));
    plat.width = w;
    plat.height = h;
    tutorial_ui_t *ui = tutorial_ui_create(&plat);
    if(!ui) { fprintf(stderr, "ui create failed (字体路径? FONT_REGISTRY_DIR)\n"); return 1; }

    for(int page = 0; !tutorial_ui_should_exit(ui); page++) {
        /* 给 VE 检测线程一点时间出结果 (host 上秒失败 → UNKNOWN) */
        for(int i = 0; i < 30; i++) {
            tutorial_ui_tick(ui);
            lv_timer_handler();
            nanosleep(&(struct timespec){ 0, 10000000 }, NULL);
        }
        lv_refr_now(NULL);
        char path[512];
        snprintf(path, sizeof(path), "%s/page%d_%dx%d.png", outdir, page + 1, w, h);
        if(!save_png(path, fb, w, h)) { fprintf(stderr, "write %s failed\n", path); return 1; }
        printf("%s\n", path);
        tutorial_ui_handle_key(ui, TUTORIAL_KEY_ENTER);
    }
    tutorial_ui_destroy(ui);
    return 0;
}
