#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **paths; /* 同目录下所有图片的绝对路径，按文件名排序 */
    size_t count;
    size_t current;
} iv_gallery_t;

bool iv_gallery_is_image(const char *path);
bool iv_gallery_is_gif(const char *path);
bool iv_gallery_load(iv_gallery_t *gallery, const char *start_path);
void iv_gallery_free(iv_gallery_t *gallery);
const char *iv_gallery_current(const iv_gallery_t *gallery);
bool iv_gallery_prev(iv_gallery_t *gallery);
bool iv_gallery_next(iv_gallery_t *gallery);
