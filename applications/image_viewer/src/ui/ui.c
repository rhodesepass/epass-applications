#include "ui.h"

/* lv_image_cache_drop 未被 lvgl.h 公开导出，按 lvgl 根目录相对路径引入 */
#include <src/misc/cache/instance/lv_image_cache.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FONT_REGISTRY_DIR
#define FONT_REGISTRY_DIR "/usr/share/fonts/epass"
#endif

struct iv_ui {
    iv_platform_t *platform;
    iv_gallery_t *gallery;

    lv_obj_t *image;
    lv_obj_t *error_label;
    lv_obj_t *status;

    lv_font_t *ui_font;
    char lvgl_src[PATH_MAX + 4]; /* "A:" 前缀 + 当前图片路径，lv_image 直接引用它 */
    bool status_visible;
    bool should_exit;
};

static int scaled(const iv_ui_t *ui, int base)
{
    int value = base * ui->platform->width / 360;
    return value > 0 ? value : 1;
}

static lv_font_t *load_font(int font_px)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/SourceHanSansSC-Regular.otf", FONT_REGISTRY_DIR);
    if(access(path, R_OK) != 0)
        snprintf(path, sizeof(path), "/usr/share/fonts/epass/SourceHanSansSC-Regular.otf");
    return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   (uint32_t)font_px, LV_FREETYPE_FONT_STYLE_NORMAL);
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void update_status(iv_ui_t *ui, const lv_image_header_t *header)
{
    const char *path = iv_gallery_current(ui->gallery);
    char text[PATH_MAX + 96];
    if(header)
        snprintf(text, sizeof(text), "%s  %zu/%zu  %ux%u", basename_of(path),
                 ui->gallery->current + 1, ui->gallery->count,
                 (unsigned)header->w, (unsigned)header->h);
    else
        snprintf(text, sizeof(text), "%s  %zu/%zu", basename_of(path),
                 ui->gallery->current + 1, ui->gallery->count);
    lv_label_set_text(ui->status, text);
}

static void show_current(iv_ui_t *ui)
{
    const char *path = iv_gallery_current(ui->gallery);
    bool is_gif = iv_gallery_is_gif(path);
    lv_image_header_t header;
    bool has_header = false;

    if(ui->image) {
        /* 立即释放上一张的解码缓存，F1C 内存有限，不等 LRU 淘汰 */
        lv_image_cache_drop(ui->lvgl_src);
        lv_obj_delete(ui->image);
        ui->image = NULL;
    }
    if(ui->error_label) {
        lv_obj_delete(ui->error_label);
        ui->error_label = NULL;
    }
    snprintf(ui->lvgl_src, sizeof(ui->lvgl_src), "A:%s", path);

    /* GIF 走 lv_gif 自己的解码器，不在 decoder 管线里，跳过探测 */
    if(!is_gif && lv_image_decoder_get_info(ui->lvgl_src, &header) != LV_RESULT_OK) {
        ui->error_label = lv_label_create(lv_screen_active());
        lv_label_set_text_fmt(ui->error_label, "无法解码\n%s", basename_of(path));
        lv_obj_set_style_text_align(ui->error_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(ui->error_label);
        update_status(ui, NULL);
        lv_obj_move_foreground(ui->status);
        return;
    }
    has_header = !is_gif;

    ui->image = is_gif ? lv_gif_create(lv_screen_active())
                       : lv_image_create(lv_screen_active());
    lv_obj_set_size(ui->image, ui->platform->width, ui->platform->height);
    lv_obj_set_pos(ui->image, 0, 0);
    lv_image_set_inner_align(ui->image, LV_IMAGE_ALIGN_CONTAIN);
    if(is_gif) lv_gif_set_src(ui->image, ui->lvgl_src);
    else lv_image_set_src(ui->image, ui->lvgl_src);
    update_status(ui, has_header ? &header : NULL);
    lv_obj_move_foreground(ui->status);
}

iv_ui_t *iv_ui_create(iv_platform_t *platform, iv_gallery_t *gallery)
{
    iv_ui_t *ui = calloc(1, sizeof(*ui));
    if(!ui) return NULL;
    ui->platform = platform;
    ui->gallery = gallery;
    ui->status_visible = true;

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_white(), 0);

    ui->ui_font = load_font(scaled(ui, 18));
    if(ui->ui_font)
        lv_obj_set_style_text_font(lv_screen_active(), ui->ui_font, 0);

    ui->status = lv_label_create(lv_screen_active());
    lv_obj_set_width(ui->status, platform->width);
    lv_obj_align(ui->status, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(ui->status, scaled(ui, 6), 0);
    lv_obj_set_style_bg_color(ui->status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->status, LV_OPA_70, 0);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_DOT);

    show_current(ui);
    return ui;
}

void iv_ui_handle_key(iv_ui_t *ui, iv_key_t key)
{
    switch(key) {
    case IV_KEY_PREV:
        if(iv_gallery_prev(ui->gallery)) show_current(ui);
        break;
    case IV_KEY_NEXT:
        if(iv_gallery_next(ui->gallery)) show_current(ui);
        break;
    case IV_KEY_ENTER:
        ui->status_visible = !ui->status_visible;
        if(ui->status_visible) lv_obj_remove_flag(ui->status, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ui->status, LV_OBJ_FLAG_HIDDEN);
        break;
    case IV_KEY_BACK:
        ui->should_exit = true;
        break;
    default:
        break;
    }
}

bool iv_ui_should_exit(const iv_ui_t *ui) { return ui->should_exit; }

void iv_ui_destroy(iv_ui_t *ui)
{
    if(!ui) return;
    if(ui->image) {
        lv_image_cache_drop(ui->lvgl_src);
        lv_obj_delete(ui->image);
    }
    if(ui->ui_font) lv_freetype_font_delete(ui->ui_font);
    free(ui);
}
