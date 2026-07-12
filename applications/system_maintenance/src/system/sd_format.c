#define _GNU_SOURCE

#include "sd_format.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SD_DEVICE "/dev/mmcblk0"
#define SD_PARTITION_FULL "/dev/mmcblk0p1"
#define SD_PARTITION_DATA "/dev/mmcblk0p3"
#define SD_MOUNTPOINT "/sd"

enum {
    CHILD_NONE = 0,
    CHILD_FDISK,
    CHILD_MKFS_FAT,
    CHILD_REFRESH
};

/* 整卡单一 FAT32 (LBA) 分区，起始 1 MiB，结束用默认值（卡末尾）。 */
static const char FDISK_SCRIPT[] =
    "o\n"
    "n\np\n1\n2048\n\n"
    "t\nc\n"
    "w\n";

static void set_error(sd_format_t *format, const char *text, ...)
{
    va_list arguments;
    if (format == NULL) return;
    format->phase = SD_FORMAT_FAILED;
    format->capable = false;
    va_start(arguments, text);
    (void)vsnprintf(format->error, sizeof(format->error), text, arguments);
    va_end(arguments);
}

static int64_t monotonic_ms(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

void sd_format_init(sd_format_t *format)
{
    if (format != NULL) {
        memset(format, 0, sizeof(*format));
        format->phase = SD_FORMAT_IDLE;
        format->child_pid = -1;
        (void)snprintf(format->error, sizeof(format->error), "尚未执行预检");
    }
}

const char *sd_format_fdisk_script(void)
{
    return FDISK_SCRIPT;
}

sd_format_mode_t sd_format_mode(const sd_format_t *format)
{
    return format == NULL ? SD_FORMAT_MODE_UNKNOWN : format->mode;
}

static int copy_path(const char *source, char *destination, size_t capacity)
{
    int written = snprintf(destination, capacity, "%s", source);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int sd_format_find_tool(const char *name, const char *path_environment,
                        char *resolved, size_t resolved_size)
{
    static const char *const prefixes[] = {
        "/sbin", "/usr/sbin", "/bin", "/usr/bin"
    };
    size_t index;
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL ||
        resolved == NULL || resolved_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
        char candidate[SD_FORMAT_PATH_MAX];
        int length = snprintf(candidate, sizeof(candidate), "%s/%s",
                              prefixes[index], name);
        if (length >= 0 && (size_t)length < sizeof(candidate) &&
            access(candidate, X_OK) == 0) {
            return copy_path(candidate, resolved, resolved_size);
        }
    }
    if (path_environment != NULL) {
        const char *cursor = path_environment;
        while (true) {
            const char *end = strchr(cursor, ':');
            size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);
            char candidate[SD_FORMAT_PATH_MAX];
            int written;
            if (length == 0U) {
                written = snprintf(candidate, sizeof(candidate), "./%s", name);
            } else {
                written = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                                   (int)length, cursor, name);
            }
            if (written >= 0 && (size_t)written < sizeof(candidate) &&
                access(candidate, X_OK) == 0) {
                return copy_path(candidate, resolved, resolved_size);
            }
            if (end == NULL) break;
            cursor = end + 1;
        }
    }
    errno = ENOENT;
    return -1;
}

bool sd_format_device_source_is_target_partition(const char *source)
{
    const char *suffix;
    if (source == NULL || strncmp(source, SD_DEVICE "p", 13U) != 0) return false;
    suffix = source + 13;
    if (*suffix == '\0') return false;
    while (*suffix != '\0') {
        if (!isdigit((unsigned char)*suffix)) return false;
        ++suffix;
    }
    return true;
}

static bool source_matches_mode(const char *decoded_source, sd_format_mode_t mode)
{
    if (mode == SD_FORMAT_MODE_DATA_PARTITION) {
        /* SD 启动时根文件系统就在 mmcblk0p2，绝不能碰其它分区。 */
        return strcmp(decoded_source, SD_PARTITION_DATA) == 0;
    }
    return strcmp(decoded_source, SD_DEVICE) == 0 ||
           sd_format_device_source_is_target_partition(decoded_source);
}

static int decode_mount_field(const char *source, size_t length,
                              char *destination, size_t capacity)
{
    size_t input = 0U;
    size_t output = 0U;
    while (input < length) {
        unsigned char value = (unsigned char)source[input++];
        if (value == '\\' && input + 2U < length &&
            source[input] >= '0' && source[input] <= '7' &&
            source[input + 1U] >= '0' && source[input + 1U] <= '7' &&
            source[input + 2U] >= '0' && source[input + 2U] <= '7') {
            value = (unsigned char)((source[input] - '0') * 64 +
                                    (source[input + 1U] - '0') * 8 +
                                    (source[input + 2U] - '0'));
            input += 3U;
        }
        if (output + 1U >= capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
        destination[output++] = (char)value;
    }
    destination[output] = '\0';
    return 0;
}

static bool mount_fields(const char *line, const char *end,
                         const char **mountpoint, size_t *mountpoint_length,
                         const char **source, size_t *source_length)
{
    const char *separator = strstr(line, " - ");
    const char *cursor = line;
    unsigned int field = 0U;
    if (separator == NULL || separator >= end) return false;
    while (cursor < separator) {
        const char *field_end = memchr(cursor, ' ', (size_t)(separator - cursor));
        if (field_end == NULL) field_end = separator;
        ++field;
        if (field == 5U) {
            *mountpoint = cursor;
            *mountpoint_length = (size_t)(field_end - cursor);
            break;
        }
        cursor = field_end < separator ? field_end + 1 : separator;
    }
    cursor = strchr(separator + 3, ' ');
    if (field != 5U || cursor == NULL || cursor >= end) return false;
    ++cursor;
    *source = cursor;
    {
        const char *source_end = memchr(cursor, ' ', (size_t)(end - cursor));
        if (source_end == NULL) source_end = end;
        *source_length = (size_t)(source_end - cursor);
    }
    return true;
}

int sd_format_mountinfo_collect(const char *mountinfo, sd_format_mode_t mode,
                                sd_format_mounts_t *mounts)
{
    const char *line;
    if (mountinfo == NULL || mounts == NULL ||
        (mode != SD_FORMAT_MODE_FULL_CARD &&
         mode != SD_FORMAT_MODE_DATA_PARTITION)) {
        errno = EINVAL;
        return -1;
    }
    memset(mounts, 0, sizeof(*mounts));
    line = mountinfo;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *mountpoint = NULL;
        const char *source = NULL;
        size_t mountpoint_length = 0U;
        size_t source_length = 0U;
        char decoded_source[SD_FORMAT_PATH_MAX];
        if (end == NULL) end = line + strlen(line);
        if (mount_fields(line, end, &mountpoint, &mountpoint_length,
                         &source, &source_length) &&
            decode_mount_field(source, source_length, decoded_source,
                               sizeof(decoded_source)) == 0 &&
            source_matches_mode(decoded_source, mode)) {
            if (mounts->count >= SD_FORMAT_MAX_MOUNTS) {
                mounts->truncated = true;
                return 0;
            }
            if (decode_mount_field(mountpoint, mountpoint_length,
                                   mounts->paths[mounts->count],
                                   sizeof(mounts->paths[mounts->count])) != 0) {
                return -1;
            }
            ++mounts->count;
        }
        line = *end == '\0' ? end : end + 1;
    }
    return 0;
}

static char *read_mountinfo(void)
{
    FILE *file = fopen("/proc/self/mountinfo", "r");
    char *buffer;
    size_t used = 0U;
    size_t capacity = 4096U;
    if (file == NULL) return NULL;
    buffer = malloc(capacity);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    while (!feof(file)) {
        size_t got;
        if (capacity - used <= 1U) {
            char *larger;
            if (capacity >= 1024U * 1024U) {
                free(buffer);
                fclose(file);
                errno = EFBIG;
                return NULL;
            }
            capacity *= 2U;
            larger = realloc(buffer, capacity);
            if (larger == NULL) {
                free(buffer);
                fclose(file);
                return NULL;
            }
            buffer = larger;
        }
        got = fread(buffer + used, 1U, capacity - used - 1U, file);
        used += got;
        if (ferror(file)) {
            free(buffer);
            fclose(file);
            return NULL;
        }
    }
    buffer[used] = '\0';
    fclose(file);
    return buffer;
}

static int resolve_tools(sd_format_t *format)
{
    const char *path = getenv("PATH");
    if (format->mode == SD_FORMAT_MODE_FULL_CARD &&
        sd_format_find_tool("fdisk", path, format->fdisk_path,
                            sizeof(format->fdisk_path)) != 0) {
        set_error(format, "找不到可执行的 fdisk");
        return -1;
    }
    if (sd_format_find_tool("mkfs.vfat", path, format->fat_tool_path,
                            sizeof(format->fat_tool_path)) != 0 &&
        sd_format_find_tool("mkdosfs", path, format->fat_tool_path,
                            sizeof(format->fat_tool_path)) != 0) {
        set_error(format, "找不到 mkfs.vfat 或 mkdosfs");
        return -1;
    }
    if (sd_format_find_tool("usbaioctl", path, format->refresh_path,
                            sizeof(format->refresh_path)) == 0) {
        format->refresh_uses_usbaioctl = true;
    } else if (sd_format_find_tool("usbctl", path, format->refresh_path,
                                   sizeof(format->refresh_path)) == 0) {
        format->refresh_uses_usbaioctl = false;
    } else {
        set_error(format, "找不到 usbaioctl 或 usbctl");
        return -1;
    }
    return 0;
}

static int check_block_device(sd_format_t *format, const char *path)
{
    struct stat device_status;
    if (stat(path, &device_status) != 0 || !S_ISBLK(device_status.st_mode) ||
        access(path, W_OK) != 0) {
        set_error(format, "%s 不存在、不是块设备或不可写", path);
        return -1;
    }
    return 0;
}

int sd_format_preflight(sd_format_t *format)
{
    boot_source_probe_t boot_probe;
    char boot_error[128];
    if (format == NULL) {
        errno = EINVAL;
        return -1;
    }
    format->phase = SD_FORMAT_PREFLIGHT;
    format->progress = 0U;
    format->capable = false;
    format->mode = SD_FORMAT_MODE_UNKNOWN;
    format->target_partition = NULL;
    format->error[0] = '\0';
    if (boot_source_probe(&boot_probe, boot_error, sizeof(boot_error)) != 0) {
        set_error(format, "%s", boot_error);
        return -1;
    }
    if (boot_probe.executable_on_sd || boot_probe.cwd_on_sd) {
        set_error(format, "程序或工作目录位于 /sd，无法格式化");
        return -1;
    }
    if (boot_probe.source == BOOT_SOURCE_SD) {
        format->mode = SD_FORMAT_MODE_DATA_PARTITION;
        format->target_partition = SD_PARTITION_DATA;
        if (check_block_device(format, SD_PARTITION_DATA) != 0) return -1;
    } else if (boot_probe.source == BOOT_SOURCE_NAND) {
        int fd;
        uint64_t device_size;
        int sector_size;
        format->mode = SD_FORMAT_MODE_FULL_CARD;
        format->target_partition = SD_PARTITION_FULL;
        if (check_block_device(format, SD_DEVICE) != 0) return -1;
        fd = open(SD_DEVICE, O_RDWR | O_CLOEXEC);
        if (fd < 0 || ioctl(fd, BLKGETSIZE64, &device_size) != 0 ||
            ioctl(fd, BLKSSZGET, &sector_size) != 0) {
            if (fd >= 0) close(fd);
            set_error(format, "无法读取 SD 卡容量或逻辑扇区大小");
            return -1;
        }
        close(fd);
        if (sector_size != 512 ||
            device_size < (uint64_t)SD_FORMAT_FULL_CARD_MIN_BYTES) {
            set_error(format, "SD 卡必须使用 512 字节逻辑扇区且容量至少 32 MiB");
            return -1;
        }
    } else {
        set_error(format, "无法识别启动来源");
        return -1;
    }
    if (resolve_tools(format) != 0) return -1;
    format->capable = true;
    format->phase = SD_FORMAT_IDLE;
    format->progress = 5U;
    (void)snprintf(format->error, sizeof(format->error), "预检通过");
    return 0;
}

static int path_depth(const char *path)
{
    int depth = 0;
    while (*path != '\0') {
        if (*path++ == '/') ++depth;
    }
    return depth;
}

static int deeper_mount_first(const void *left, const void *right)
{
    const char *left_path = left;
    const char *right_path = right;
    int left_depth = path_depth(left_path);
    int right_depth = path_depth(right_path);
    if (left_depth != right_depth) return right_depth - left_depth;
    if (strlen(left_path) < strlen(right_path)) return 1;
    if (strlen(left_path) > strlen(right_path)) return -1;
    return 0;
}

static pid_t spawn_process(const char *path, char *const arguments[],
                           const char *standard_input)
{
    int input_pipe[2] = {-1, -1};
    pid_t child;
    if (standard_input != NULL && pipe(input_pipe) != 0) return -1;
    child = fork();
    if (child == 0) {
        if (standard_input != NULL) {
            (void)close(input_pipe[1]);
            if (dup2(input_pipe[0], STDIN_FILENO) < 0) _exit(126);
            (void)close(input_pipe[0]);
        }
        execv(path, arguments);
        _exit(127);
    }
    if (child < 0) {
        if (standard_input != NULL) {
            (void)close(input_pipe[0]);
            (void)close(input_pipe[1]);
        }
        return -1;
    }
    if (standard_input != NULL) {
        const char *cursor = standard_input;
        size_t remaining = strlen(standard_input);
        struct sigaction ignore_action;
        struct sigaction old_action;
        (void)close(input_pipe[0]);
        memset(&ignore_action, 0, sizeof(ignore_action));
        ignore_action.sa_handler = SIG_IGN;
        (void)sigaction(SIGPIPE, &ignore_action, &old_action);
        while (remaining > 0U) {
            ssize_t written = write(input_pipe[1], cursor, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            cursor += written;
            remaining -= (size_t)written;
        }
        (void)close(input_pipe[1]);
        (void)sigaction(SIGPIPE, &old_action, NULL);
    }
    return child;
}

static int spawn_fat(sd_format_t *format)
{
    char *fat32_arguments[] = {
        format->fat_tool_path, "-F", "32", "-n", "EPASS",
        (char *)format->target_partition, NULL
    };
    char *fat_auto_arguments[] = {
        format->fat_tool_path, "-n", "EPASS",
        (char *)format->target_partition, NULL
    };
    char **arguments = format->mode == SD_FORMAT_MODE_FULL_CARD ?
                       fat32_arguments : fat_auto_arguments;
    format->child_pid = spawn_process(format->fat_tool_path, arguments, NULL);
    if (format->child_pid < 0) return -1;
    format->destructive = true;
    format->child_kind = CHILD_MKFS_FAT;
    format->phase = SD_FORMAT_CREATING_FILESYSTEM;
    format->progress = 55U;
    return 0;
}

int sd_format_start(sd_format_t *format)
{
    char *mountinfo;
    size_t index;
    if (format == NULL ||
        (format->phase != SD_FORMAT_IDLE && format->phase != SD_FORMAT_FAILED &&
         format->phase != SD_FORMAT_COMPLETE &&
         format->phase != SD_FORMAT_CANCELLED)) {
        errno = EBUSY;
        return -1;
    }
    sd_format_init(format);
    if (sd_format_preflight(format) != 0) return -1;
    mountinfo = read_mountinfo();
    if (mountinfo == NULL ||
        sd_format_mountinfo_collect(mountinfo, format->mode,
                                    &format->mounts) != 0 ||
        format->mounts.truncated) {
        free(mountinfo);
        set_error(format, "无法安全枚举 SD 卡挂载点");
        return -1;
    }
    free(mountinfo);
    qsort(format->mounts.paths, format->mounts.count,
          sizeof(format->mounts.paths[0]), deeper_mount_first);
    format->phase = SD_FORMAT_UNMOUNTING;
    format->progress = 10U;
    for (index = 0U; index < format->mounts.count; ++index) {
        if (umount2(format->mounts.paths[index], 0) != 0) {
            set_error(format, "卸载 %s 失败: %s",
                      format->mounts.paths[index], strerror(errno));
            return -1;
        }
    }
    if (format->mode == SD_FORMAT_MODE_DATA_PARTITION) {
        if (spawn_fat(format) != 0) {
            set_error(format, "无法启动 FAT 格式化工具");
            return -1;
        }
        return 0;
    }
    {
        char *arguments[3];
        arguments[0] = format->fdisk_path;
        arguments[1] = (char *)SD_DEVICE;
        arguments[2] = NULL;
        format->child_pid =
            spawn_process(format->fdisk_path, arguments, FDISK_SCRIPT);
    }
    if (format->child_pid < 0) {
        set_error(format, "无法启动 fdisk");
        return -1;
    }
    format->destructive = true;
    format->child_kind = CHILD_FDISK;
    format->phase = SD_FORMAT_PARTITIONING;
    format->progress = 20U;
    return 0;
}

static int create_text_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const char *cursor = contents;
    size_t remaining = strlen(contents);
    if (fd < 0) return -1;
    while (remaining > 0U) {
        ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(fd);
            return -1;
        }
        cursor += written;
        remaining -= (size_t)written;
    }
    {
        int sync_result = fsync(fd);
        int close_result = close(fd);
        return sync_result == 0 && close_result == 0 ? 0 : -1;
    }
}

static int populate_without_marker(void)
{
    (void)unlink(SD_MOUNTPOINT "/.epass_sd");
    if (mkdir(SD_MOUNTPOINT "/assets", 0755) != 0 && errno != EEXIST) return -1;
    return create_text_file(
        SD_MOUNTPOINT "/README.txt",
        "电子通行证 SD 卡数据分区\n\n"
        "该分区为 FAT 数据分区，可直接放置应用与素材文件。\n");
}

static int spawn_refresh(sd_format_t *format)
{
    char *aio_arguments[] = {format->refresh_path, "select", "mtp", NULL};
    char *usbctl_arguments[] = {format->refresh_path, "mtp", NULL};
    char **arguments = format->refresh_uses_usbaioctl ?
                       aio_arguments : usbctl_arguments;
    format->child_pid = spawn_process(format->refresh_path, arguments, NULL);
    if (format->child_pid < 0) return -1;
    format->child_kind = CHILD_REFRESH;
    format->phase = SD_FORMAT_REFRESHING_MTP;
    format->progress = 95U;
    return 0;
}

static void after_child(sd_format_t *format, int child_kind)
{
    if (child_kind == CHILD_FDISK) {
        int fd = open(SD_DEVICE, O_RDONLY | O_CLOEXEC);
        if (fd < 0 || ioctl(fd, BLKRRPART) != 0) {
            if (fd >= 0) close(fd);
            set_error(format, "BLKRRPART 失败: %s", strerror(errno));
            return;
        }
        close(fd);
        format->phase = SD_FORMAT_WAITING_PARTITION;
        format->progress = 40U;
        format->deadline_ms = monotonic_ms() + 10000;
    } else if (child_kind == CHILD_MKFS_FAT) {
        format->phase = SD_FORMAT_MOUNTING;
        format->progress = 85U;
        if (mkdir(SD_MOUNTPOINT, 0755) != 0 && errno != EEXIST) {
            set_error(format, "无法创建 /sd: %s", strerror(errno));
        } else if (mount(format->target_partition, SD_MOUNTPOINT, "vfat",
                         MS_NOATIME, "utf8=1") != 0) {
            set_error(format, "挂载 /sd 失败: %s", strerror(errno));
        } else {
            format->phase = SD_FORMAT_POPULATING;
            format->progress = 90U;
            if (populate_without_marker() != 0) {
                set_error(format, "创建 assets/README 失败: %s", strerror(errno));
            } else if (spawn_refresh(format) != 0) {
                set_error(format, "无法刷新 MTP");
            }
        }
    } else if (child_kind == CHILD_REFRESH) {
        if (create_text_file(SD_MOUNTPOINT "/.epass_sd",
                             "epass-sd-layout-v1\n") != 0 ||
            create_text_file(SD_MOUNTPOINT "/.epass_sd", "") != 0) {
            (void)unlink(SD_MOUNTPOINT "/.epass_sd");
            set_error(format, "创建完成标记失败: %s", strerror(errno));
        } else {
            format->phase = SD_FORMAT_COMPLETE;
            format->progress = 100U;
            format->capable = true;
            format->error[0] = '\0';
        }
    }
}

void sd_format_poll(sd_format_t *format)
{
    int status;
    pid_t waited;
    int child_kind;
    if (format == NULL || sd_format_finished(format)) return;
    if (format->phase == SD_FORMAT_WAITING_PARTITION) {
        struct stat partition_status;
        if (stat(format->target_partition, &partition_status) == 0 &&
            S_ISBLK(partition_status.st_mode)) {
            if (spawn_fat(format) != 0)
                set_error(format, "无法启动 FAT 格式化工具");
        } else if (monotonic_ms() >= format->deadline_ms) {
            set_error(format, "等待 SD 分区节点超时");
        }
        return;
    }
    if (format->child_pid <= 0) return;
    waited = waitpid(format->child_pid, &status, WNOHANG);
    if (waited == 0) return;
    if (waited < 0) {
        if (errno != EINTR) set_error(format, "waitpid 失败: %s", strerror(errno));
        return;
    }
    child_kind = format->child_kind;
    format->child_pid = -1;
    format->child_kind = CHILD_NONE;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(format, "外部工具执行失败（wait 状态 %d）", status);
        return;
    }
    after_child(format, child_kind);
}

bool sd_format_cancel(sd_format_t *format)
{
    if (format == NULL || format->destructive) return false;
    if (format->phase == SD_FORMAT_IDLE || format->phase == SD_FORMAT_PREFLIGHT) {
        format->phase = SD_FORMAT_CANCELLED;
        return true;
    }
    return false;
}

bool sd_format_capable(const sd_format_t *format)
{
    return format != NULL && format->capable;
}

const char *sd_format_capability_reason(const sd_format_t *format)
{
    return format == NULL ? "无效状态" : format->error;
}

sd_format_phase_t sd_format_phase(const sd_format_t *format)
{
    return format == NULL ? SD_FORMAT_FAILED : format->phase;
}

unsigned int sd_format_progress(const sd_format_t *format)
{
    return format == NULL ? 0U : format->progress;
}

const char *sd_format_error(const sd_format_t *format)
{
    return format == NULL ? "无效状态" : format->error;
}

bool sd_format_finished(const sd_format_t *format)
{
    return format == NULL || format->phase == SD_FORMAT_COMPLETE ||
           format->phase == SD_FORMAT_FAILED ||
           format->phase == SD_FORMAT_CANCELLED;
}
