#ifndef SYSTEM_MAINTENANCE_SD_FORMAT_H
#define SYSTEM_MAINTENANCE_SD_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "boot_source.h"

#define SD_FORMAT_PATH_MAX 4096U
#define SD_FORMAT_MAX_MOUNTS 32U
#define SD_FORMAT_FULL_CARD_MIN_BYTES (32U * 1024U * 1024U)

typedef enum {
    SD_FORMAT_MODE_UNKNOWN = 0,
    /* NAND 启动：整卡重建为单个 FAT32 分区 (mmcblk0p1) */
    SD_FORMAT_MODE_FULL_CARD,
    /* SD 启动：仅把数据分区 mmcblk0p3 重新格式化为 FAT */
    SD_FORMAT_MODE_DATA_PARTITION
} sd_format_mode_t;

typedef enum {
    SD_FORMAT_IDLE = 0,
    SD_FORMAT_PREFLIGHT,
    SD_FORMAT_UNMOUNTING,
    SD_FORMAT_PARTITIONING,
    SD_FORMAT_WAITING_PARTITION,
    SD_FORMAT_CREATING_FILESYSTEM,
    SD_FORMAT_MOUNTING,
    SD_FORMAT_POPULATING,
    SD_FORMAT_REFRESHING_MTP,
    SD_FORMAT_COMPLETE,
    SD_FORMAT_FAILED,
    SD_FORMAT_CANCELLED
} sd_format_phase_t;

typedef struct {
    char paths[SD_FORMAT_MAX_MOUNTS][SD_FORMAT_PATH_MAX];
    size_t count;
    bool truncated;
} sd_format_mounts_t;

typedef struct {
    sd_format_phase_t phase;
    sd_format_mode_t mode;
    unsigned int progress;
    bool destructive;
    bool capable;
    pid_t child_pid;
    int child_kind;
    int64_t deadline_ms;
    const char *target_partition;
    char fdisk_path[SD_FORMAT_PATH_MAX];
    char fat_tool_path[SD_FORMAT_PATH_MAX];
    char refresh_path[SD_FORMAT_PATH_MAX];
    bool refresh_uses_usbaioctl;
    sd_format_mounts_t mounts;
    char error[256];
} sd_format_t;

void sd_format_init(sd_format_t *format);
int sd_format_mountinfo_collect(const char *mountinfo, sd_format_mode_t mode,
                                sd_format_mounts_t *mounts);
bool sd_format_device_source_is_target_partition(const char *source);
const char *sd_format_fdisk_script(void);
int sd_format_find_tool(const char *name, const char *path_environment,
                        char *resolved, size_t resolved_size);
bool sd_format_capable(const sd_format_t *format);
const char *sd_format_capability_reason(const sd_format_t *format);
sd_format_mode_t sd_format_mode(const sd_format_t *format);

int sd_format_preflight(sd_format_t *format);
int sd_format_start(sd_format_t *format);
void sd_format_poll(sd_format_t *format);
bool sd_format_cancel(sd_format_t *format);
sd_format_phase_t sd_format_phase(const sd_format_t *format);
unsigned int sd_format_progress(const sd_format_t *format);
const char *sd_format_error(const sd_format_t *format);
bool sd_format_finished(const sd_format_t *format);

#endif
