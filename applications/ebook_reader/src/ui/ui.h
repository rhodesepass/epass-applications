#pragma once

#include "../port/platform.h"
#include "../reader/reader.h"

typedef struct ebook_ui ebook_ui_t;

ebook_ui_t *ebook_ui_create(ebook_platform_t *platform, ebook_document_t *document,
                            ebook_state_t *state);
void ebook_ui_destroy(ebook_ui_t *ui);
void ebook_ui_handle_key(ebook_ui_t *ui, ebook_key_t key);
bool ebook_ui_should_exit(const ebook_ui_t *ui);
void ebook_ui_save_position(ebook_ui_t *ui);
