#pragma once

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EBOOK_MAX_BOOKMARKS 16
#define EBOOK_SUMMARY_LEN 64

typedef enum {
    EBOOK_THEME_DAY = 0,
    EBOOK_THEME_NIGHT,
    EBOOK_THEME_EYE
} ebook_theme_t;

typedef struct {
    size_t offset;
    char summary[EBOOK_SUMMARY_LEN];
} ebook_bookmark_t;

typedef struct {
    uint64_t file_size;
    size_t position;
    int font_px;
    ebook_theme_t theme;
    int brightness;
    int bookmark_count;
    ebook_bookmark_t bookmarks[EBOOK_MAX_BOOKMARKS];
} ebook_state_t;

typedef struct {
    char *text;
    size_t length;
    uint64_t source_size;
    char path[1024];
    char title[256];
    size_t *pages;
    size_t page_count;
    size_t page_capacity;
    size_t *line_breaks;
    size_t line_break_count;
    size_t line_break_capacity;
    size_t current_page;
} ebook_document_t;

bool ebook_load_document(ebook_document_t *doc, const char *path);
void ebook_free_document(ebook_document_t *doc);
bool ebook_paginate(ebook_document_t *doc, const lv_font_t *font,
                    int body_width, int body_height, int line_height);
size_t ebook_page_for_offset(const ebook_document_t *doc, size_t offset);
char *ebook_page_text(const ebook_document_t *doc, size_t page);
void ebook_make_summary(const ebook_document_t *doc, size_t offset,
                        char out[EBOOK_SUMMARY_LEN]);

void ebook_state_defaults(ebook_state_t *state);
bool ebook_state_load(const ebook_document_t *doc, ebook_state_t *state);
bool ebook_state_save(const ebook_document_t *doc, const ebook_state_t *state);
bool ebook_state_add_bookmark(const ebook_document_t *doc, ebook_state_t *state,
                              size_t offset);
void ebook_state_delete_bookmark(ebook_state_t *state, int index);
