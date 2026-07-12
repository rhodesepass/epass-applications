#include "reader.h"
#include "gbk_table.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool utf8_next(const char *s, size_t len, size_t *at, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = *at;
    uint32_t value;
    int extra;
    if(i >= len) return false;
    if(p[i] < 0x80) {
        *cp = p[i++];
        *at = i;
        return true;
    }
    if(p[i] >= 0xC2 && p[i] <= 0xDF) {
        value = p[i] & 0x1F; extra = 1;
    } else if(p[i] >= 0xE0 && p[i] <= 0xEF) {
        value = p[i] & 0x0F; extra = 2;
    } else if(p[i] >= 0xF0 && p[i] <= 0xF4) {
        value = p[i] & 0x07; extra = 3;
    } else return false;
    if(i + (size_t)extra >= len) return false;
    for(int n = 1; n <= extra; n++) {
        unsigned char b = p[i + n];
        if((b & 0xC0) != 0x80) return false;
        value = (value << 6) | (b & 0x3F);
    }
    if((extra == 2 && value < 0x800) || (extra == 3 && value < 0x10000) ||
       (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) return false;
    *at = i + (size_t)extra + 1;
    *cp = value;
    return true;
}

static bool valid_utf8(const char *s, size_t len)
{
    size_t at = 0;
    uint32_t cp;
    while(at < len) if(!utf8_next(s, len, &at, &cp)) return false;
    return true;
}

uint32_t ebook_gbk_lookup(uint16_t gbk)
{
    size_t lo = 0, hi = ebook_gbk_table_count;
    while(lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if(ebook_gbk_table[mid].gbk < gbk) lo = mid + 1;
        else hi = mid;
    }
    return lo < ebook_gbk_table_count && ebook_gbk_table[lo].gbk == gbk
        ? ebook_gbk_table[lo].unicode : 0xFFFD;
}

static size_t put_utf8(char *out, uint32_t cp)
{
    if(cp < 0x80) { out[0] = (char)cp; return 1; }
    if(cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if(cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static char *gbk_to_utf8(const unsigned char *src, size_t len, size_t *out_len)
{
    char *out = malloc(len * 3 + 1);
    size_t i = 0, used = 0;
    if(!out) return NULL;
    while(i < len) {
        uint32_t cp;
        if(src[i] < 0x80) cp = src[i++];
        else if(src[i] == 0x80) { cp = 0x20AC; i++; }
        else if(i + 1 < len) {
            cp = ebook_gbk_lookup((uint16_t)((src[i] << 8) | src[i + 1]));
            i += 2;
        } else { cp = 0xFFFD; i++; }
        used += put_utf8(out + used, cp);
    }
    out[used] = '\0';
    *out_len = used;
    return out;
}

bool ebook_load_document(ebook_document_t *doc, const char *path)
{
    FILE *fp;
    long size;
    unsigned char *raw;
    char resolved[PATH_MAX];
    memset(doc, 0, sizeof(*doc));
    if(!realpath(path, resolved)) return false;
    fp = fopen(resolved, "rb");
    if(!fp) return false;
    if(fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp); return false;
    }
    raw = malloc((size_t)size + 1);
    if(!raw) { fclose(fp); return false; }
    if(fread(raw, 1, (size_t)size, fp) != (size_t)size) {
        free(raw); fclose(fp); return false;
    }
    fclose(fp);
    raw[size] = 0;
    size_t skip = size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
    if(valid_utf8((char *)raw + skip, (size_t)size - skip)) {
        doc->length = (size_t)size - skip;
        doc->text = malloc(doc->length + 1);
        if(doc->text) memcpy(doc->text, raw + skip, doc->length + 1);
    } else doc->text = gbk_to_utf8(raw + skip, (size_t)size - skip, &doc->length);
    free(raw);
    if(!doc->text) return false;
    doc->source_size = (uint64_t)size;
    if(strlen(resolved) >= sizeof(doc->path)) {
        ebook_free_document(doc);
        return false;
    }
    memcpy(doc->path, resolved, strlen(resolved) + 1);
    const char *base = strrchr(resolved, '/');
    const char *title = base ? base + 1 : resolved;
    size_t title_len = strlen(title);
    if(title_len >= sizeof(doc->title)) title_len = sizeof(doc->title) - 1;
    memcpy(doc->title, title, title_len);
    doc->title[title_len] = '\0';
    return true;
}

void ebook_free_document(ebook_document_t *doc)
{
    free(doc->text);
    free(doc->pages);
    free(doc->line_breaks);
    memset(doc, 0, sizeof(*doc));
}

static bool push_page(ebook_document_t *doc, size_t offset)
{
    if(doc->page_count == doc->page_capacity) {
        size_t capacity = doc->page_capacity ? doc->page_capacity * 2 : 128;
        size_t *pages = realloc(doc->pages, capacity * sizeof(*pages));
        if(!pages) return false;
        doc->pages = pages;
        doc->page_capacity = capacity;
    }
    doc->pages[doc->page_count++] = offset;
    return true;
}

static bool push_line_break(ebook_document_t *doc, size_t offset)
{
    if(doc->line_break_count == doc->line_break_capacity) {
        size_t capacity = doc->line_break_capacity ? doc->line_break_capacity * 2 : 256;
        size_t *breaks = realloc(doc->line_breaks, capacity * sizeof(*breaks));
        if(!breaks) return false;
        doc->line_breaks = breaks;
        doc->line_break_capacity = capacity;
    }
    doc->line_breaks[doc->line_break_count++] = offset;
    return true;
}

typedef struct {
    uint32_t codepoint;
    int advance;
    bool valid;
} advance_cache_entry_t;

static int glyph_advance(const lv_font_t *font, uint32_t cp, int fallback,
                         advance_cache_entry_t cache[1024])
{
    advance_cache_entry_t *entry = &cache[cp % 1024];
    if(entry->valid && entry->codepoint == cp) return entry->advance;
    lv_font_glyph_dsc_t glyph;
    entry->codepoint = cp;
    entry->advance = lv_font_get_glyph_dsc(font, &glyph, cp, 0) ? glyph.adv_w : fallback;
    entry->valid = true;
    return entry->advance;
}

static bool forbidden_line_start(uint32_t cp)
{
    switch(cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E:
    case 0xFF01: case 0xFF1F: case 0xFF1A: case 0xFF1B:
    case 0x2019: case 0x201D: case 0x3009: case 0x300B:
    case 0x300D: case 0x300F: case 0x3011: case 0x3015:
    case 0x3017: case 0x3019: case 0x301B: case 0xFF09:
    case 0xFF3D: case 0xFF5D:
        return true;
    default:
        return false;
    }
}

bool ebook_paginate(ebook_document_t *doc, const lv_font_t *font,
                    int body_width, int body_height, int line_height)
{
    size_t at = 0, line_start = 0;
    int x = 0, y = 0;
    uint32_t cp;
    advance_cache_entry_t cache[1024] = {{0}};
    doc->page_count = 0;
    doc->line_break_count = 0;
    if(!push_page(doc, 0)) return false;
    while(at < doc->length) {
        size_t before = at;
        if(!utf8_next(doc->text, doc->length, &at, &cp)) { at++; continue; }
        if(cp == '\r') continue;
        if(cp == '\n') { x = 0; y += line_height; line_start = at; }
        else {
            int advance = glyph_advance(font, cp, line_height / 2, cache);
            if(x > 0 && x + advance > body_width && !forbidden_line_start(cp)) {
                if(!push_line_break(doc, before)) return false;
                x = 0; y += line_height; line_start = before;
            }
            x += advance;
        }
        if(y + line_height > body_height) {
            size_t page_at = line_start > doc->pages[doc->page_count - 1] ? line_start : before;
            if(page_at <= doc->pages[doc->page_count - 1]) page_at = at;
            if(!push_page(doc, page_at)) return false;
            y = 0;
            x = page_at == at ? 0 : x;
            line_start = page_at;
        }
    }
    if(doc->current_page >= doc->page_count) doc->current_page = doc->page_count - 1;
    return true;
}

size_t ebook_page_for_offset(const ebook_document_t *doc, size_t offset)
{
    size_t lo = 0, hi = doc->page_count;
    while(lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if(doc->pages[mid] <= offset) lo = mid; else hi = mid;
    }
    return lo;
}

char *ebook_page_text(const ebook_document_t *doc, size_t page)
{
    size_t start = doc->pages[page];
    size_t end = page + 1 < doc->page_count ? doc->pages[page + 1] : doc->length;
    size_t extra = 0;
    for(size_t i = 0; i < doc->line_break_count; i++)
        if(doc->line_breaks[i] > start && doc->line_breaks[i] < end) extra++;
    char *text = malloc(end - start + extra + 1);
    if(!text) return NULL;
    size_t source = start, used = 0, break_index = 0;
    while(break_index < doc->line_break_count && doc->line_breaks[break_index] <= start)
        break_index++;
    while(source < end) {
        if(break_index < doc->line_break_count &&
           doc->line_breaks[break_index] == source) {
            text[used++] = '\n';
            break_index++;
        }
        text[used++] = doc->text[source++];
    }
    text[used] = '\0';
    return text;
}

void ebook_make_summary(const ebook_document_t *doc, size_t offset, char out[EBOOK_SUMMARY_LEN])
{
    size_t used = 0, at = offset;
    while(at < doc->length && used + 4 < EBOOK_SUMMARY_LEN) {
        size_t before = at; uint32_t cp;
        if(!utf8_next(doc->text, doc->length, &at, &cp)) { at++; continue; }
        if(cp == '\r' || cp == '\n' || cp == '\t') cp = ' ';
        size_t bytes = at - before;
        if(cp == ' ') { out[used++] = ' '; }
        else { memcpy(out + used, doc->text + before, bytes); used += bytes; }
    }
    out[used] = '\0';
}

static uint64_t path_hash(const char *s)
{
    uint64_t h = UINT64_C(1469598103934665603);
    while(*s) { h ^= (unsigned char)*s++; h *= UINT64_C(1099511628211); }
    return h;
}

static void state_path(const ebook_document_t *doc, char out[384])
{
    char basename[128];
    size_t used = 0;
    const char *name = strrchr(doc->path, '/');
    name = name ? name + 1 : doc->path;
    while(*name && used + 1 < sizeof(basename)) {
        unsigned char c = (unsigned char)*name++;
        basename[used++] = (c >= 0x80 || c == '/' || c == '\\') ? '_' : (char)c;
    }
    basename[used] = '\0';
    mkdir("./ebook_state", 0755);
    snprintf(out, 384, "./ebook_state/%016llx_%s.state",
             (unsigned long long)path_hash(doc->path), basename);
}

void ebook_state_defaults(ebook_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->font_px = 20;
    state->theme = EBOOK_THEME_DAY;
    state->brightness = 5;
}

bool ebook_state_load(const ebook_document_t *doc, ebook_state_t *state)
{
    char path[384], line[512];
    uint64_t saved_size = 0;
    FILE *fp;
    ebook_state_defaults(state);
    state_path(doc, path);
    fp = fopen(path, "r");
    if(!fp) return false;
    while(fgets(line, sizeof(line), fp)) {
        char *value = strchr(line, '=');
        if(!value) continue;
        *value++ = '\0';
        value[strcspn(value, "\r\n")] = '\0';
        if(strcmp(line, "file_size") == 0)
            saved_size = strtoull(value, NULL, 10);
        else if(strcmp(line, "position") == 0)
            state->position = (size_t)strtoull(value, NULL, 10);
        else if(strcmp(line, "font_px") == 0)
            state->font_px = atoi(value);
        else if(strcmp(line, "theme") == 0)
            state->theme = (ebook_theme_t)atoi(value);
        else if(strcmp(line, "brightness") == 0)
            state->brightness = atoi(value);
        else if(strcmp(line, "bookmark") == 0 &&
                state->bookmark_count < EBOOK_MAX_BOOKMARKS) {
            char *separator = strchr(value, '|');
            if(!separator) continue;
            *separator++ = '\0';
            ebook_bookmark_t *mark = &state->bookmarks[state->bookmark_count++];
            mark->offset = (size_t)strtoull(value, NULL, 10);
            snprintf(mark->summary, sizeof(mark->summary), "%s", separator);
        }
    }
    fclose(fp);
    state->file_size = saved_size;
    if(state->font_px != 18 && state->font_px != 20 &&
       state->font_px != 22 && state->font_px != 26) state->font_px = 20;
    if(state->theme < EBOOK_THEME_DAY || state->theme > EBOOK_THEME_EYE)
        state->theme = EBOOK_THEME_DAY;
    if(state->brightness < 1 || state->brightness > 9) state->brightness = 5;
    if(saved_size != doc->source_size) {
        state->position = 0;
        state->bookmark_count = 0;
    } else if(state->position > doc->length) {
        state->position = 0;
    }
    return true;
}

bool ebook_state_save(const ebook_document_t *doc, const ebook_state_t *state)
{
    char path[384], temp[400];
    FILE *fp;
    state_path(doc, path);
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    fp = fopen(temp, "w");
    if(!fp) return false;
    bool ok = fprintf(fp,
        "version=1\nfile_size=%llu\nposition=%zu\nfont_px=%d\ntheme=%d\nbrightness=%d\n",
        (unsigned long long)doc->source_size, state->position, state->font_px,
        (int)state->theme, state->brightness) > 0;
    for(int i = 0; ok && i < state->bookmark_count; i++)
        ok = fprintf(fp, "bookmark=%zu|%s\n", state->bookmarks[i].offset,
                     state->bookmarks[i].summary) > 0;
    ok = ok && fflush(fp) == 0;
    if(fclose(fp) != 0) ok = false;
    if(ok) ok = rename(temp, path) == 0; else unlink(temp);
    return ok;
}

bool ebook_state_add_bookmark(const ebook_document_t *doc, ebook_state_t *state, size_t offset)
{
    for(int i = 0; i < state->bookmark_count; i++)
        if(state->bookmarks[i].offset == offset) return false;
    if(state->bookmark_count == EBOOK_MAX_BOOKMARKS) {
        memmove(&state->bookmarks[0], &state->bookmarks[1],
                sizeof(state->bookmarks[0]) * (EBOOK_MAX_BOOKMARKS - 1));
        state->bookmark_count--;
    }
    ebook_bookmark_t *mark = &state->bookmarks[state->bookmark_count++];
    mark->offset = offset;
    ebook_make_summary(doc, offset, mark->summary);
    return true;
}

void ebook_state_delete_bookmark(ebook_state_t *state, int index)
{
    if(index < 0 || index >= state->bookmark_count) return;
    memmove(&state->bookmarks[index], &state->bookmarks[index + 1],
            sizeof(state->bookmarks[0]) * (state->bookmark_count - index - 1));
    state->bookmark_count--;
}
