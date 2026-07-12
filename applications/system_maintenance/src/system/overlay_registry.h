#ifndef SYSTEM_MAINTENANCE_OVERLAY_REGISTRY_H
#define SYSTEM_MAINTENANCE_OVERLAY_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OVERLAY_CATEGORY_INTERFACE = 0,
    OVERLAY_CATEGORY_EXTENSION
} overlay_category_t;

typedef enum {
    OVERLAY_DEPENDENCY_ALL = 0,
    OVERLAY_DEPENDENCY_ANY
} overlay_dependency_mode_t;

typedef struct {
    const char *id;
    overlay_category_t category;
    const char *name_zh;
    const char *description_zh;
    int min_revision_rank;
    int max_revision_rank;
    const char *const *requires;
    size_t requires_count;
    overlay_dependency_mode_t dependency_mode;
    const char *const *conflicts;
    size_t conflicts_count;
} overlay_registry_item_t;

typedef struct {
    const overlay_registry_item_t *items;
    size_t count;
} overlay_registry_t;

const overlay_registry_t *overlay_registry_get(void);
const overlay_registry_item_t *overlay_registry_find(const char *id);
int overlay_revision_rank(const char *device_revision);
bool overlay_registry_available(const overlay_registry_item_t *item,
                                const char *device_revision);

#endif
