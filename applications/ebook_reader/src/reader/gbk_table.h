#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t gbk;
    uint32_t unicode;
} gbk_map_entry_t;

extern const gbk_map_entry_t ebook_gbk_table[];
extern const size_t ebook_gbk_table_count;

uint32_t ebook_gbk_lookup(uint16_t gbk);
