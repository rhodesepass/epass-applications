#include "niccc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NICCC_SRC_W = 256,
    NICCC_SRC_H = 200,
    NICCC_SCALED_W = 460,
    NICCC_SCALED_H = 360,
    NICCC_ROT_W = 360,   // after 90deg CCW
    NICCC_ROT_H = 460,
};

typedef struct {
    uint32_t offset;           // offset in stream where the frame starts (flags byte)
    uint8_t flags;             // original flags byte
    uint32_t palette_argb[16]; // palette snapshot AFTER applying this frame's palette updates
} niccc_frame_meta_t;

static uint8_t* g_stream = NULL;
static size_t g_stream_size = 0;

static niccc_frame_meta_t* g_frames = NULL;
static int g_frame_count = 0;
static bool g_index_ready = false;

static uint32_t g_scaled_fb[NICCC_SCALED_W * NICCC_SCALED_H];
static int g_last_rendered_frame = -1;

static inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint8_t expand_3_to_8(uint8_t v3) {
    // map 0..7 to 0..255
    return (uint8_t)((v3 * 255u + 3u) / 7u);
}

static inline uint32_t atari_st_to_argb(uint16_t st) {
    // 00000RRR0GGG0BBB
    const uint8_t r3 = (uint8_t)((st >> 8) & 0x7);
    const uint8_t g3 = (uint8_t)((st >> 4) & 0x7);
    const uint8_t b3 = (uint8_t)(st & 0x7);
    const uint8_t r8 = expand_3_to_8(r3);
    const uint8_t g8 = expand_3_to_8(g3);
    const uint8_t b8 = expand_3_to_8(b3);
    return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8;
}

static inline int scale_x_256_to_460(uint8_t x) {
    // x' = round(x * (W-1) / 255) where W=460
    return (int)(((uint32_t)x * (NICCC_SCALED_W - 1) + 127u) / 255u);
}

static inline int scale_y_200_to_360(uint8_t y) {
    // y' = round(y * (H-1) / 199) where H=360
    return (int)(((uint32_t)y * (NICCC_SCALED_H - 1) + 99u) / 199u);
}

static void fill_black_scaledfb(void) {
    for (int i = 0; i < NICCC_SCALED_W * NICCC_SCALED_H; i++) {
        g_scaled_fb[i] = 0xFF000000u;
    }
}

static inline int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void fill_polygon_even_odd(uint32_t* fb, int W, int H, const int* xs, const int* ys, int n, uint32_t color) {
    if (!fb || !xs || !ys) return;
    if (W <= 0 || H <= 0) return;
    if (n < 3) return;

    int miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    miny = clamp_int(miny, 0, H - 1);
    maxy = clamp_int(maxy, 0, H - 1);
    if (miny > maxy) return;

    int xints[32];

    // For each scanline y, collect intersections, sort, then fill between pairs.
    for (int y = miny; y <= maxy; y++) {
        int cnt = 0;
        const int64_t y_fp = ((int64_t)y << 16) + 0x8000; // y + 0.5 in 16.16

        for (int i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            const int x0 = xs[i], y0 = ys[i];
            const int x1 = xs[j], y1 = ys[j];

            if (y0 == y1) continue; // ignore horizontal edges

            const int ymin = y0 < y1 ? y0 : y1;
            const int ymax = y0 < y1 ? y1 : y0;

            // half-open: include y in [ymin, ymax)
            if (y < ymin || y >= ymax) continue;

            const int dy = (y1 - y0);
            const int dx = (x1 - x0);

            const int64_t y0_fp = (int64_t)y0 << 16;
            const int64_t x0_fp = (int64_t)x0 << 16;
            const int64_t t_fp = y_fp - y0_fp;

            // x = x0 + (y - y0) * dx / dy
            const int64_t x_fp = x0_fp + (t_fp * (int64_t)dx) / (int64_t)dy;
            int xi = (int)(x_fp >> 16);

            if (cnt < (int)(sizeof(xints) / sizeof(xints[0]))) {
                xints[cnt++] = xi;
            }
        }

        if (cnt < 2) continue;

        // insertion sort (cnt is small: <= edges)
        for (int i = 1; i < cnt; i++) {
            int key = xints[i];
            int k = i - 1;
            while (k >= 0 && xints[k] > key) {
                xints[k + 1] = xints[k];
                k--;
            }
            xints[k + 1] = key;
        }

        for (int i = 0; i + 1 < cnt; i += 2) {
            int x0 = xints[i];
            int x1 = xints[i + 1];
            if (x0 > x1) {
                const int tmp = x0;
                x0 = x1;
                x1 = tmp;
            }

            // Fill [x0, x1) (x1 excluded)
            int start = clamp_int(x0, 0, W);
            int end = clamp_int(x1, 0, W);
            if (start >= end) continue;

            uint32_t* row = fb + y * W;
            for (int x = start; x < end; x++) {
                row[x] = color;
            }
        }
    }
}

static inline bool need_bytes(size_t off, size_t need) {
    return g_stream && (off + need) <= g_stream_size;
}

static size_t align_next_64k(size_t off) {
    return (off + 0xFFFFu) & ~0xFFFFu;
}

static bool build_frame_index_once(void) {
    if (g_index_ready) return true;

    if (!g_stream || g_stream_size < 1) return false;

    // initial palette = 0
    uint16_t pal_st[16] = {0};

    int cap = 2048;
    g_frames = (niccc_frame_meta_t*)malloc((size_t)cap * sizeof(niccc_frame_meta_t));
    if (!g_frames) return false;

    size_t off = 0;
    int frame_count = 0;
    bool done = false;

    while (!done && off < g_stream_size) {
        if (!need_bytes(off, 1)) break;
        const uint8_t flags = g_stream[off];

        if (frame_count >= cap) {
            cap *= 2;
            niccc_frame_meta_t* nf = (niccc_frame_meta_t*)realloc(g_frames, (size_t)cap * sizeof(niccc_frame_meta_t));
            if (!nf) break;
            g_frames = nf;
        }

        g_frames[frame_count].offset = (uint32_t)off;
        g_frames[frame_count].flags = flags;

        size_t p = off + 1;

        // palette update block
        if (flags & 0x02) {
            if (!need_bytes(p, 2)) break;
            const uint16_t mask = read_be16(g_stream + p);
            p += 2;
            for (int b = 0; b < 16; b++) {
                if ((mask >> b) & 1u) {
                    if (!need_bytes(p, 2)) { done = true; break; }
                    const uint16_t col = read_be16(g_stream + p);
                    p += 2;
                    const int idx = 15 - b; // reverse index
                    pal_st[idx] = col;
                }
            }
            if (done) break;
        }

        // snapshot palette after applying updates of this frame
        for (int i = 0; i < 16; i++) {
            g_frames[frame_count].palette_argb[i] = atari_st_to_argb(pal_st[i]);
        }

        const bool indexed = (flags & 0x04) != 0;
        if (indexed) {
            if (!need_bytes(p, 1)) break;
            const uint8_t nverts = g_stream[p++];
            // skip vertex table
            const size_t vbytes = (size_t)nverts * 2u;
            if (!need_bytes(p, vbytes)) break;
            p += vbytes;

            // polygons: descriptor + vertex ids
            while (1) {
                if (!need_bytes(p, 1)) { done = true; break; }
                const uint8_t desc = g_stream[p++];
                if (desc == 0xFF) {
                    off = p;
                    break;
                }
                if (desc == 0xFE) {
                    off = align_next_64k(p);
                    break;
                }
                if (desc == 0xFD) {
                    done = true;
                    off = p;
                    break;
                }
                const int vcnt = (int)(desc & 0x0F);
                if (vcnt < 3 || vcnt > 15) { done = true; break; }
                if (!need_bytes(p, (size_t)vcnt)) { done = true; break; }
                p += (size_t)vcnt;
            }
            if (done) {
                frame_count++;
                break;
            }
        } else {
            // non-indexed polygons: descriptor + (x,y)*vcnt
            while (1) {
                if (!need_bytes(p, 1)) { done = true; break; }
                const uint8_t desc = g_stream[p++];
                if (desc == 0xFF) {
                    off = p;
                    break;
                }
                if (desc == 0xFE) {
                    off = align_next_64k(p);
                    break;
                }
                if (desc == 0xFD) {
                    done = true;
                    off = p;
                    break;
                }
                const int vcnt = (int)(desc & 0x0F);
                if (vcnt < 3 || vcnt > 15) { done = true; break; }
                const size_t need = (size_t)vcnt * 2u;
                if (!need_bytes(p, need)) { done = true; break; }
                p += need;
            }
            if (done) {
                frame_count++;
                break;
            }
        }

        frame_count++;

        // off is already updated by terminator. guard against stalling
        if (frame_count > 5000) break;
    }

    g_frame_count = frame_count;
    g_index_ready = (g_frame_count > 0);
    g_last_rendered_frame = -1;
    fill_black_scaledfb();
    return g_index_ready;
}

static void render_one_frame_to_scaledfb(int frame_i) {
    if (!g_index_ready) return;
    if (frame_i < 0 || frame_i >= g_frame_count) return;

    const niccc_frame_meta_t* meta = &g_frames[frame_i];
    size_t p = (size_t)meta->offset;
    if (!need_bytes(p, 1)) return;
    const uint8_t flags = g_stream[p++];

    // clear to black if requested
    if (flags & 0x01) {
        fill_black_scaledfb();
    }

    // skip palette update block (we use snapshot palette)
    if (flags & 0x02) {
        if (!need_bytes(p, 2)) return;
        const uint16_t mask = read_be16(g_stream + p);
        p += 2;
        for (int b = 0; b < 16; b++) {
            if ((mask >> b) & 1u) {
                if (!need_bytes(p, 2)) return;
                p += 2;
            }
        }
    }

    const bool indexed = (flags & 0x04) != 0;

    if (indexed) {
        if (!need_bytes(p, 1)) return;
        const uint8_t nverts = g_stream[p++];
        if (!need_bytes(p, (size_t)nverts * 2u)) return;

        int vx[256];
        int vy[256];
        for (int i = 0; i < (int)nverts; i++) {
            const uint8_t x = g_stream[p++];
            const uint8_t y = g_stream[p++];
            vx[i] = scale_x_256_to_460(x);
            vy[i] = scale_y_200_to_360(y);
        }

        int px[15];
        int py[15];

        while (1) {
            if (!need_bytes(p, 1)) return;
            const uint8_t desc = g_stream[p++];
            if (desc == 0xFF || desc == 0xFE || desc == 0xFD) {
                return;
            }
            const int color_idx = (desc >> 4) & 0x0F;
            const int vcnt = (int)(desc & 0x0F);
            if (vcnt < 3 || vcnt > 15) return;
            if (!need_bytes(p, (size_t)vcnt)) return;

            for (int i = 0; i < vcnt; i++) {
                const uint8_t id = g_stream[p++];
                const int vid = (id < nverts) ? (int)id : 0;
                px[i] = vx[vid];
                py[i] = vy[vid];
            }

            const uint32_t color = meta->palette_argb[color_idx];
            fill_polygon_even_odd(g_scaled_fb, NICCC_SCALED_W, NICCC_SCALED_H, px, py, vcnt, color);
        }
    } else {
        int px[15];
        int py[15];

        while (1) {
            if (!need_bytes(p, 1)) return;
            const uint8_t desc = g_stream[p++];
            if (desc == 0xFF || desc == 0xFE || desc == 0xFD) {
                return;
            }
            const int color_idx = (desc >> 4) & 0x0F;
            const int vcnt = (int)(desc & 0x0F);
            if (vcnt < 3 || vcnt > 15) return;
            if (!need_bytes(p, (size_t)vcnt * 2u)) return;

            for (int i = 0; i < vcnt; i++) {
                const uint8_t x = g_stream[p++];
                const uint8_t y = g_stream[p++];
                px[i] = scale_x_256_to_460(x);
                py[i] = scale_y_200_to_360(y);
            }

            const uint32_t color = meta->palette_argb[color_idx];
            fill_polygon_even_odd(g_scaled_fb, NICCC_SCALED_W, NICCC_SCALED_H, px, py, vcnt, color);
        }
    }
}

static void blit_scaledfb_ccw(game_framebuffer_t* fb) {
    if (!fb || !fb->pixels || fb->pitch < fb->width * 4) return;
    const int dst_y0 = game_logical_y(fb, GAME_LOGICAL_HEIGHT - NICCC_ROT_H);
    for (int y = dst_y0; y < fb->height; y++) {
        uint32_t* row = (uint32_t*)((uint8_t*)fb->pixels +
                                   (size_t)y * (size_t)fb->pitch);
        int logical_y = (int)((int64_t)y * GAME_LOGICAL_HEIGHT /
                              fb->height);
        int rotated_y = logical_y -
                        (GAME_LOGICAL_HEIGHT - NICCC_ROT_H);
        if (rotated_y < 0 || rotated_y >= NICCC_ROT_H) continue;
        int sx = NICCC_SCALED_W - 1 - rotated_y;
        for (int x = 0; x < fb->width; x++) {
            int sy = (int)((int64_t)x * NICCC_SCALED_H / fb->width);
            if (sy >= NICCC_SCALED_H) sy = NICCC_SCALED_H - 1;
            row[x] = g_scaled_fb[sy * NICCC_SCALED_W + sx];
        }
    }
}

bool niccc_init(const char* scene_path) {
    if (!scene_path || g_stream) return false;
    FILE* file = fopen(scene_path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    g_stream = (uint8_t*)malloc((size_t)length);
    if (!g_stream) {
        fclose(file);
        return false;
    }
    g_stream_size = fread(g_stream, 1, (size_t)length, file);
    fclose(file);
    if (g_stream_size != (size_t)length || !build_frame_index_once()) {
        niccc_destroy();
        return false;
    }
    return true;
}

void niccc_draw_frame(game_framebuffer_t* framebuffer, int frame_idx){
    if (!framebuffer) return;
    if (!build_frame_index_once()) return;
    if (g_frame_count <= 0) return;

    if (frame_idx < 0) frame_idx = 0;
    if (frame_idx >= g_frame_count) frame_idx = g_frame_count - 1;

    if (frame_idx <= g_last_rendered_frame) {
        // replay to keep correct cumulative rendering when frames don't clear
        g_last_rendered_frame = -1;
        fill_black_scaledfb();
    }

    for (int i = g_last_rendered_frame + 1; i <= frame_idx; i++) {
        render_one_frame_to_scaledfb(i);
        g_last_rendered_frame = i;
    }

    blit_scaledfb_ccw(framebuffer);
}

void niccc_destroy(void) {
    free(g_frames);
    free(g_stream);
    g_frames = NULL;
    g_stream = NULL;
    g_stream_size = 0;
    g_frame_count = 0;
    g_index_ready = false;
    g_last_rendered_frame = -1;
}
