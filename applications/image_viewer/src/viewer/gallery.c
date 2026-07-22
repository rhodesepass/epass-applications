#include "gallery.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *extensions[] = {".jpg", ".jpeg", ".png", ".bmp", ".gif"};
#define EXTENSION_COUNT (sizeof(extensions) / sizeof(extensions[0]))

static const char *path_extension(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *dot = strrchr(base ? base + 1 : path, '.');
    return dot;
}

static bool extension_equals(const char *ext, const char *wanted)
{
    while(*ext && *wanted) {
        if(tolower((unsigned char)*ext) != (unsigned char)*wanted) return false;
        ext++;
        wanted++;
    }
    return *ext == '\0' && *wanted == '\0';
}

bool iv_gallery_is_image(const char *path)
{
    const char *ext = path_extension(path);
    if(!ext) return false;
    for(size_t i = 0; i < EXTENSION_COUNT; i++)
        if(extension_equals(ext, extensions[i])) return true;
    return false;
}

bool iv_gallery_is_gif(const char *path)
{
    const char *ext = path_extension(path);
    return ext && extension_equals(ext, ".gif");
}

static int compare_paths(const void *a, const void *b)
{
    /* 同一目录下，比整条路径等价于比文件名 */
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

bool iv_gallery_load(iv_gallery_t *gallery, const char *start_path)
{
    char resolved[PATH_MAX];
    char dir[PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    memset(gallery, 0, sizeof(*gallery));
    if(!realpath(start_path, resolved)) return false;
    strcpy(dir, resolved);
    char *slash = strrchr(dir, '/');
    if(!slash) return false;
    if(slash == dir) slash[1] = '\0';
    else *slash = '\0';
    handle = opendir(dir);
    if(!handle) return false;
    size_t capacity = 0;
    while((entry = readdir(handle)) != NULL) {
        char full[PATH_MAX];
        int written;
        if(entry->d_name[0] == '.') continue;
        if(!iv_gallery_is_image(entry->d_name)) continue;
        written = snprintf(full, sizeof(full), "%s/%s",
                           strcmp(dir, "/") == 0 ? "" : dir, entry->d_name);
        if(written <= 0 || (size_t)written >= sizeof(full)) continue;
        if(gallery->count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            char **grown = realloc(gallery->paths, capacity * sizeof(char *));
            if(!grown) goto fail;
            gallery->paths = grown;
        }
        gallery->paths[gallery->count] = strdup(full);
        if(!gallery->paths[gallery->count]) goto fail;
        gallery->count++;
    }
    closedir(handle);
    handle = NULL;
    /* 启动路径始终入册(扩展名由系统关联,应用不拒收) */
    {
        bool found = false;
        for(size_t i = 0; i < gallery->count; i++) {
            if(strcmp(gallery->paths[i], resolved) == 0) {
                found = true;
                break;
            }
        }
        if(!found) {
            if(gallery->count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                char **grown = realloc(gallery->paths, capacity * sizeof(char *));
                if(!grown) goto fail;
                gallery->paths = grown;
            }
            gallery->paths[gallery->count] = strdup(resolved);
            if(!gallery->paths[gallery->count]) goto fail;
            gallery->count++;
        }
    }
    if(gallery->count == 0) goto fail;
    qsort(gallery->paths, gallery->count, sizeof(char *), compare_paths);
    gallery->current = 0;
    for(size_t i = 0; i < gallery->count; i++) {
        if(strcmp(gallery->paths[i], resolved) == 0) {
            gallery->current = i;
            break;
        }
    }
    return true;
fail:
    if(handle) closedir(handle);
    iv_gallery_free(gallery);
    return false;
}

void iv_gallery_free(iv_gallery_t *gallery)
{
    for(size_t i = 0; i < gallery->count; i++) free(gallery->paths[i]);
    free(gallery->paths);
    memset(gallery, 0, sizeof(*gallery));
}

const char *iv_gallery_current(const iv_gallery_t *gallery)
{
    return gallery->paths[gallery->current];
}

bool iv_gallery_prev(iv_gallery_t *gallery)
{
    if(gallery->current == 0) return false;
    gallery->current--;
    return true;
}

bool iv_gallery_next(iv_gallery_t *gallery)
{
    if(gallery->current + 1 >= gallery->count) return false;
    gallery->current++;
    return true;
}
