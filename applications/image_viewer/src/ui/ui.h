#pragma once

#include "../port/platform.h"
#include "../viewer/gallery.h"

typedef struct iv_ui iv_ui_t;

iv_ui_t *iv_ui_create(iv_platform_t *platform, iv_gallery_t *gallery);
void iv_ui_destroy(iv_ui_t *ui);
void iv_ui_handle_key(iv_ui_t *ui, iv_key_t key);
bool iv_ui_should_exit(const iv_ui_t *ui);
