#define _POSIX_C_SOURCE 200809L

#include "boot_config.h"
#include "boot_source.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_reason(char *destination, size_t size, const char *text)
{
    if (destination != NULL && size > 0U) {
        (void)snprintf(destination, size, "%s", text);
    }
}

void boot_config_init(boot_config_t *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->backend = BOOT_CONFIG_BACKEND_NONE;
        set_reason(config->write_reason, sizeof(config->write_reason),
                   "尚未加载启动配置");
    }
}

static bool valid_key(const char *key)
{
    const unsigned char *cursor = (const unsigned char *)key;
    if (key == NULL || *key == '\0') return false;
    while (*cursor != '\0') {
        if (*cursor == '=' || isspace(*cursor) || *cursor < 0x21U ||
            *cursor > 0x7eU) {
            return false;
        }
        ++cursor;
    }
    return true;
}

const char *boot_config_get(const boot_config_t *config, const char *key)
{
    size_t index;
    if (config == NULL || key == NULL) return NULL;
    for (index = 0U; index < config->entry_count; ++index) {
        if (strcmp(config->entries[index].key, key) == 0) {
            return config->entries[index].value;
        }
    }
    return NULL;
}

static int set_entry(boot_config_t *config, const char *key, size_t key_length,
                     const char *value, size_t value_length)
{
    size_t index;
    if (key_length == 0U || key_length >= BOOT_CONFIG_MAX_KEY_LENGTH ||
        value_length >= BOOT_CONFIG_MAX_VALUE_LENGTH) {
        errno = EOVERFLOW;
        return -1;
    }
    for (index = 0U; index < config->entry_count; ++index) {
        if (strlen(config->entries[index].key) == key_length &&
            memcmp(config->entries[index].key, key, key_length) == 0) {
            memcpy(config->entries[index].value, value, value_length);
            config->entries[index].value[value_length] = '\0';
            return 0;
        }
    }
    if (config->entry_count >= BOOT_CONFIG_MAX_ENTRIES) {
        errno = ENOSPC;
        return -1;
    }
    index = config->entry_count++;
    memcpy(config->entries[index].key, key, key_length);
    config->entries[index].key[key_length] = '\0';
    memcpy(config->entries[index].value, value, value_length);
    config->entries[index].value[value_length] = '\0';
    return 0;
}

static void parse_tokens(const char *value, boot_config_tokens_t *tokens)
{
    const char *cursor = value;
    while (*cursor != '\0') {
        const char *end;
        size_t length;
        while (isspace((unsigned char)*cursor)) ++cursor;
        end = cursor;
        while (*end != '\0' && !isspace((unsigned char)*end)) ++end;
        length = (size_t)(end - cursor);
        if (length > 0U) {
            if (tokens->count >= BOOT_CONFIG_MAX_TOKENS) {
                tokens->truncated = true;
                return;
            }
            if (length >= BOOT_CONFIG_MAX_TOKEN_LENGTH) {
                length = BOOT_CONFIG_MAX_TOKEN_LENGTH - 1U;
                tokens->truncated = true;
            }
            memcpy(tokens->values[tokens->count], cursor, length);
            tokens->values[tokens->count][length] = '\0';
            ++tokens->count;
        }
        cursor = end;
    }
}

static void refresh_fields(boot_config_t *config)
{
    const char *value;
    memset(config->device_rev, 0, sizeof(config->device_rev));
    memset(config->screen, 0, sizeof(config->screen));
    memset(&config->interfaces, 0, sizeof(config->interfaces));
    memset(&config->extensions, 0, sizeof(config->extensions));
    value = boot_config_get(config, "device_rev");
    if (value != NULL) {
        (void)snprintf(config->device_rev, sizeof(config->device_rev), "%s", value);
    }
    value = boot_config_get(config, "screen");
    if (value != NULL) {
        (void)snprintf(config->screen, sizeof(config->screen), "%s", value);
    }
    value = boot_config_get(config, "interface");
    if (value != NULL) parse_tokens(value, &config->interfaces);
    value = boot_config_get(config, "ext");
    if (value != NULL) parse_tokens(value, &config->extensions);
    if (config->interfaces.truncated || config->extensions.truncated) {
        config->malformed = true;
    }
}

int boot_config_parse_buffer(const uint8_t *buffer, size_t length,
                             boot_config_t *config)
{
    size_t text_length = 0U;
    size_t cursor = 0U;
    if (buffer == NULL || config == NULL) {
        errno = EINVAL;
        return -1;
    }
    boot_config_init(config);
    while (text_length < length && buffer[text_length] != '\0') ++text_length;
    if (text_length == length) {
        config->malformed = true;
        errno = EINVAL;
        return -1;
    }
    while (cursor < text_length) {
        size_t end = cursor;
        size_t equals;
        while (end < text_length && buffer[end] != '\n' && buffer[end] != '\r') ++end;
        equals = cursor;
        while (equals < end && buffer[equals] != '=') ++equals;
        if (end > cursor) {
            if (equals == cursor || equals == end ||
                set_entry(config, (const char *)buffer + cursor, equals - cursor,
                          (const char *)buffer + equals + 1U,
                          end - equals - 1U) != 0) {
                config->malformed = true;
                return -1;
            }
        }
        cursor = end;
        while (cursor < text_length &&
               (buffer[cursor] == '\n' || buffer[cursor] == '\r')) ++cursor;
    }
    refresh_fields(config);
    return 0;
}

int boot_config_serialize(const boot_config_t *config, uint8_t *buffer,
                          size_t capacity, size_t *serialized_length)
{
    size_t index;
    size_t used = 0U;
    if (config == NULL || buffer == NULL || capacity == 0U) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0U; index < config->entry_count; ++index) {
        size_t key_length = strlen(config->entries[index].key);
        size_t value_length = strlen(config->entries[index].value);
        if (!valid_key(config->entries[index].key) ||
            strchr(config->entries[index].value, '\n') != NULL ||
            strchr(config->entries[index].value, '\r') != NULL ||
            used + key_length + value_length + 2U >= capacity) {
            errno = EINVAL;
            return -1;
        }
        memcpy(buffer + used, config->entries[index].key, key_length);
        used += key_length;
        buffer[used++] = '=';
        memcpy(buffer + used, config->entries[index].value, value_length);
        used += value_length;
        buffer[used++] = '\n';
    }
    buffer[used++] = '\0';
    if (serialized_length != NULL) *serialized_length = used;
    return 0;
}

int boot_config_set(boot_config_t *config, const char *key, const char *value)
{
    if (config == NULL || !valid_key(key) || value == NULL ||
        strchr(value, '\n') != NULL || strchr(value, '\r') != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (set_entry(config, key, strlen(key), value, strlen(value)) != 0) return -1;
    refresh_fields(config);
    return 0;
}

int boot_config_remove(boot_config_t *config, const char *key)
{
    size_t index;
    if (config == NULL || key == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0U; index < config->entry_count; ++index) {
        if (strcmp(config->entries[index].key, key) == 0) {
            memmove(&config->entries[index], &config->entries[index + 1U],
                    (config->entry_count - index - 1U) *
                    sizeof(config->entries[0]));
            --config->entry_count;
            refresh_fields(config);
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

int boot_config_find_bootenv_mtd(const char *sys_class_mtd,
                                 char *device_path, size_t path_size)
{
    DIR *directory;
    struct dirent *entry;
    if (sys_class_mtd == NULL || device_path == NULL || path_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    directory = opendir(sys_class_mtd);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL) {
        char name_path[512];
        char label[64];
        FILE *file;
        size_t length;
        int written;
        const char *suffix;
        if (strncmp(entry->d_name, "mtd", 3U) != 0) continue;
        suffix = entry->d_name + 3;
        if (*suffix == '\0') continue;
        while (isdigit((unsigned char)*suffix)) ++suffix;
        if (*suffix != '\0') continue;
        written = snprintf(name_path, sizeof(name_path), "%s/%s/name",
                           sys_class_mtd, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(name_path)) continue;
        file = fopen(name_path, "r");
        if (file == NULL) continue;
        length = fread(label, 1U, sizeof(label) - 1U, file);
        fclose(file);
        while (length > 0U && (label[length - 1U] == '\n' ||
                              label[length - 1U] == '\r')) --length;
        label[length] = '\0';
        if (strcmp(label, "bootenv") == 0) {
            written = snprintf(device_path, path_size, "/dev/%s", entry->d_name);
            closedir(directory);
            if (written < 0 || (size_t)written >= path_size) {
                errno = ENAMETOOLONG;
                return -1;
            }
            return 0;
        }
    }
    closedir(directory);
    errno = ENOENT;
    return -1;
}

bool boot_config_validate_layout(const boot_config_mtd_layout_t *layout,
                                 char *reason, size_t reason_size)
{
    if (layout == NULL) {
        set_reason(reason, reason_size, "没有 MTD 布局信息");
        return false;
    }
    if (layout->type != MTD_NANDFLASH) {
        set_reason(reason, reason_size, "bootenv 目标不是 NAND MTD");
        return false;
    }
    if (layout->size != BOOT_CONFIG_EXPECTED_MTD_SIZE ||
        layout->erase_size != BOOT_CONFIG_EXPECTED_ERASE_SIZE ||
        layout->write_size != BOOT_CONFIG_EXPECTED_WRITE_SIZE ||
        BOOT_CONFIG_ENV_OFFSET % layout->erase_size != 0U ||
        BOOT_CONFIG_ENV_ERASE_LENGTH != layout->erase_size ||
        BOOT_CONFIG_ENV_DATA_LENGTH % layout->write_size != 0U ||
        BOOT_CONFIG_ENV_OFFSET + BOOT_CONFIG_ENV_ERASE_LENGTH > layout->size) {
        set_reason(reason, reason_size, "MTD 大小、擦除块或页大小与上游布局不符");
        return false;
    }
    if ((layout->flags & MTD_WRITEABLE) == 0U) {
        set_reason(reason, reason_size, "Linux 将 bootenv MTD 标记为只读");
        return false;
    }
    set_reason(reason, reason_size, "布局及写权限验证通过");
    return true;
}

static int get_layout(int fd, boot_config_mtd_layout_t *layout)
{
    struct mtd_info_user info;
    if (ioctl(fd, MEMGETINFO, &info) != 0) return -1;
    layout->size = info.size;
    layout->erase_size = info.erasesize;
    layout->write_size = info.writesize;
    layout->type = info.type;
    layout->flags = info.flags;
    return 0;
}

static int read_exact_at(int fd, uint8_t *buffer, size_t length, off_t offset)
{
    size_t used = 0U;
    while (used < length) {
        ssize_t got = pread(fd, buffer + used, length - used,
                            offset + (off_t)used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (got == 0) errno = EIO;
            return -1;
        }
        used += (size_t)got;
    }
    return 0;
}

static int load_mtd(boot_config_t *config, char *error, size_t error_size)
{
    char device[128];
    uint8_t *buffer;
    int fd;
    boot_config_mtd_layout_t layout;
    bool writable;
    char reason[256];
    if (boot_config_find_bootenv_mtd("/sys/class/mtd", device, sizeof(device)) != 0) {
        set_reason(error, error_size, "未找到标签为 bootenv 的 MTD");
        return -1;
    }
    fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || get_layout(fd, &layout) != 0) {
        if (fd >= 0) close(fd);
        set_reason(error, error_size, "无法读取 bootenv MTD 信息");
        return -1;
    }
    writable = boot_config_validate_layout(&layout, reason, sizeof(reason));
    if (!writable) {
        boot_config_mtd_layout_t readable = layout;
        readable.flags |= MTD_WRITEABLE;
        if (!boot_config_validate_layout(&readable, NULL, 0U)) {
            close(fd);
            set_reason(error, error_size, reason);
            return -1;
        }
    }
    buffer = malloc(BOOT_CONFIG_ENV_DATA_LENGTH);
    if (buffer == NULL) {
        close(fd);
        return -1;
    }
    if (read_exact_at(fd, buffer, BOOT_CONFIG_ENV_DATA_LENGTH,
                      (off_t)BOOT_CONFIG_ENV_OFFSET) != 0 ||
        boot_config_parse_buffer(buffer, BOOT_CONFIG_ENV_DATA_LENGTH, config) != 0) {
        free(buffer);
        close(fd);
        set_reason(error, error_size, "环境文本读取或解析失败");
        return -1;
    }
    free(buffer);
    close(fd);
    if (config->malformed) {
        writable = false;
        set_reason(reason, sizeof(reason),
                   "环境中的 Overlay token 超出安全解析上限，禁止覆盖保存");
    }
    config->can_write = writable;
    config->backend = BOOT_CONFIG_BACKEND_MTD;
    (void)snprintf(config->write_reason, sizeof(config->write_reason), "%s", reason);
    (void)snprintf(config->device_path, sizeof(config->device_path), "%s", device);
    return 0;
}

/*
 * ---- SD 启动后端 ----
 * U-Boot 的 load1env_select 从 mmc 0:1 (FAT 启动分区) 加载 env.txt；
 * SD 启动时改这个文件才有效，bootenv MTD 即使存在也不会被读取。
 * 启动分区平时不挂载，这里按需临时挂到私有挂载点。
 */

static int sd_env_mount(bool writable, char *error, size_t error_size)
{
    unsigned long flags = MS_NOATIME | (writable ? 0UL : (unsigned long)MS_RDONLY);
    if (mkdir(BOOT_CONFIG_SD_ENV_MOUNTPOINT, 0755) != 0 && errno != EEXIST) {
        set_reason(error, error_size, "无法创建启动分区挂载点");
        return -1;
    }
    if (mount(BOOT_CONFIG_SD_BOOT_PARTITION, BOOT_CONFIG_SD_ENV_MOUNTPOINT,
              "vfat", flags, "") != 0) {
        set_reason(error, error_size, writable
                   ? "无法以读写方式挂载启动分区"
                   : "无法挂载启动分区");
        return -1;
    }
    return 0;
}

static void sd_env_unmount(void)
{
    (void)umount2(BOOT_CONFIG_SD_ENV_MOUNTPOINT, 0);
}

static int load_sd(boot_config_t *config, char *error, size_t error_size)
{
    struct stat file_status;
    uint8_t *buffer;
    bool writable = true;
    char reason[256];
    int fd;

    set_reason(reason, sizeof(reason), "启动分区 env.txt 可读写");
    if (sd_env_mount(true, error, error_size) != 0) {
        writable = false;
        set_reason(reason, sizeof(reason), "启动分区无法以读写方式挂载");
        if (sd_env_mount(false, error, error_size) != 0) return -1;
    }
    fd = open(BOOT_CONFIG_SD_ENV_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &file_status) != 0 ||
        !S_ISREG(file_status.st_mode) ||
        (uint64_t)file_status.st_size >= BOOT_CONFIG_ENV_DATA_LENGTH) {
        if (fd >= 0) close(fd);
        sd_env_unmount();
        set_reason(error, error_size, "启动分区中没有有效的 env.txt");
        return -1;
    }
    buffer = malloc((size_t)file_status.st_size + 1U);
    if (buffer == NULL) {
        close(fd);
        sd_env_unmount();
        return -1;
    }
    if (read_exact_at(fd, buffer, (size_t)file_status.st_size, 0) != 0) {
        free(buffer);
        close(fd);
        sd_env_unmount();
        set_reason(error, error_size, "读取 env.txt 失败");
        return -1;
    }
    close(fd);
    sd_env_unmount();
    buffer[file_status.st_size] = '\0';
    if (boot_config_parse_buffer(buffer, (size_t)file_status.st_size + 1U,
                                 config) != 0) {
        free(buffer);
        set_reason(error, error_size, "环境文本读取或解析失败");
        return -1;
    }
    free(buffer);
    if (config->malformed) {
        writable = false;
        set_reason(reason, sizeof(reason),
                   "环境中的 Overlay token 超出安全解析上限，禁止覆盖保存");
    }
    config->can_write = writable;
    config->backend = BOOT_CONFIG_BACKEND_SD_FILE;
    (void)snprintf(config->write_reason, sizeof(config->write_reason), "%s", reason);
    (void)snprintf(config->device_path, sizeof(config->device_path), "%s",
                   BOOT_CONFIG_SD_ENV_FILE);
    return 0;
}

static int write_text_file(const char *path, const uint8_t *text, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    size_t used = 0U;
    if (fd < 0) return -1;
    while (used < length) {
        ssize_t written = write(fd, text + used, length - used);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)written;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int save_sd(boot_config_t *config, char *error, size_t error_size)
{
    uint8_t *expected;
    uint8_t *actual;
    size_t serialized_length;
    size_t text_length;
    struct stat file_status;
    int fd;

    expected = malloc(BOOT_CONFIG_ENV_DATA_LENGTH);
    actual = malloc(BOOT_CONFIG_ENV_DATA_LENGTH);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        return -1;
    }
    if (boot_config_serialize(config, expected, BOOT_CONFIG_ENV_DATA_LENGTH,
                              &serialized_length) != 0) {
        free(expected);
        free(actual);
        set_reason(error, error_size, "环境内容无效或超过 0x6000");
        return -1;
    }
    /* 序列化结果带结尾 NUL；env.txt 是纯文本，不写入这个 NUL */
    text_length = serialized_length - 1U;
    if (sd_env_mount(true, error, error_size) != 0) {
        config->can_write = false;
        set_reason(config->write_reason, sizeof(config->write_reason),
                   "启动分区无法以读写方式挂载");
        free(expected);
        free(actual);
        return -1;
    }
    if (write_text_file(BOOT_CONFIG_SD_ENV_FILE_TMP, expected, text_length) != 0 ||
        rename(BOOT_CONFIG_SD_ENV_FILE_TMP, BOOT_CONFIG_SD_ENV_FILE) != 0) {
        (void)unlink(BOOT_CONFIG_SD_ENV_FILE_TMP);
        sd_env_unmount();
        free(expected);
        free(actual);
        set_reason(error, error_size, "写入 env.txt 失败");
        return -1;
    }
    /* 最终的 umount 会把 FAT 元数据刷回介质 */
    fd = open(BOOT_CONFIG_SD_ENV_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &file_status) != 0 ||
        (size_t)file_status.st_size != text_length ||
        read_exact_at(fd, actual, text_length, 0) != 0 ||
        memcmp(expected, actual, text_length) != 0) {
        if (fd >= 0) close(fd);
        sd_env_unmount();
        free(expected);
        free(actual);
        set_reason(error, error_size, "env.txt 读回校验失败");
        return -1;
    }
    close(fd);
    sd_env_unmount();
    free(expected);
    free(actual);
    set_reason(error, error_size, "");
    return 0;
}

int boot_config_load(boot_config_t *config, char *error, size_t error_size)
{
    FILE *file;
    char cmdline[1024];
    size_t length;
    boot_source_t source;
    if (config == NULL) {
        errno = EINVAL;
        return -1;
    }
    file = fopen("/proc/cmdline", "r");
    if (file == NULL) {
        set_reason(error, error_size, "无法读取 /proc/cmdline");
        return -1;
    }
    length = fread(cmdline, 1U, sizeof(cmdline) - 1U, file);
    fclose(file);
    cmdline[length] = '\0';
    source = boot_source_from_cmdline(cmdline);
    if (source == BOOT_SOURCE_SD) {
        return load_sd(config, error, error_size);
    }
    return load_mtd(config, error, error_size);
}

static int write_exact_pages(int fd, const uint8_t *buffer, size_t length,
                             size_t page_size, off_t offset)
{
    size_t used;
    for (used = 0U; used < length; used += page_size) {
        ssize_t written;
        do {
            written = pwrite(fd, buffer + used, page_size, offset + (off_t)used);
        } while (written < 0 && errno == EINTR);
        if (written != (ssize_t)page_size) {
            if (written >= 0) errno = EIO;
            return -1;
        }
    }
    return 0;
}

static int save_mtd(boot_config_t *config, char *error, size_t error_size)
{
    int fd;
    boot_config_mtd_layout_t layout;
    struct erase_info_user erase;
    int64_t bad_offset = BOOT_CONFIG_ENV_OFFSET;
    uint8_t *expected;
    uint8_t *actual;
    size_t serialized_length;
    expected = malloc(BOOT_CONFIG_ENV_DATA_LENGTH);
    actual = malloc(BOOT_CONFIG_ENV_DATA_LENGTH);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        return -1;
    }
    memset(expected, 0xff, BOOT_CONFIG_ENV_DATA_LENGTH);
    if (boot_config_serialize(config, expected, BOOT_CONFIG_ENV_DATA_LENGTH,
                              &serialized_length) != 0) {
        (void)serialized_length;
        free(expected);
        free(actual);
        set_reason(error, error_size, "环境内容无效或超过 0x6000");
        return -1;
    }
    fd = open(config->device_path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0 || get_layout(fd, &layout) != 0 ||
        !boot_config_validate_layout(&layout, config->write_reason,
                                     sizeof(config->write_reason))) {
        if (fd >= 0) close(fd);
        config->can_write = false;
        free(expected);
        free(actual);
        set_reason(error, error_size, config->write_reason);
        return -1;
    }
    if (ioctl(fd, MEMGETBADBLOCK, &bad_offset) != 0) {
        close(fd);
        free(expected);
        free(actual);
        set_reason(error, error_size, "环境所在擦除块损坏或无法检查");
        return -1;
    }
    erase.start = BOOT_CONFIG_ENV_OFFSET;
    erase.length = BOOT_CONFIG_ENV_ERASE_LENGTH;
    if (ioctl(fd, MEMERASE, &erase) != 0 ||
        write_exact_pages(fd, expected, BOOT_CONFIG_ENV_DATA_LENGTH,
                          layout.write_size, (off_t)BOOT_CONFIG_ENV_OFFSET) != 0 ||
        fsync(fd) != 0 ||
        read_exact_at(fd, actual, BOOT_CONFIG_ENV_DATA_LENGTH,
                      (off_t)BOOT_CONFIG_ENV_OFFSET) != 0 ||
        memcmp(expected, actual, BOOT_CONFIG_ENV_DATA_LENGTH) != 0) {
        close(fd);
        free(expected);
        free(actual);
        set_reason(error, error_size, "擦除、按页写入或读回校验失败");
        return -1;
    }
    close(fd);
    free(expected);
    free(actual);
    set_reason(error, error_size, "");
    return 0;
}

int boot_config_save(boot_config_t *config, char *error, size_t error_size)
{
    if (config == NULL || !config->can_write || config->device_path[0] == '\0') {
        errno = EPERM;
        set_reason(error, error_size, config == NULL ? "无效配置" :
                   config->write_reason);
        return -1;
    }
    if (config->backend == BOOT_CONFIG_BACKEND_SD_FILE) {
        return save_sd(config, error, error_size);
    }
    return save_mtd(config, error, error_size);
}

bool boot_config_can_write(const boot_config_t *config)
{
    return config != NULL && config->can_write;
}

const char *boot_config_write_reason(const boot_config_t *config)
{
    return config == NULL ? "无效配置" : config->write_reason;
}

boot_config_backend_t boot_config_backend(const boot_config_t *config)
{
    return config == NULL ? BOOT_CONFIG_BACKEND_NONE : config->backend;
}

const char *boot_config_backend_name(boot_config_backend_t backend)
{
    switch (backend) {
    case BOOT_CONFIG_BACKEND_MTD: return "bootenv MTD";
    case BOOT_CONFIG_BACKEND_SD_FILE: return "SD 启动分区 env.txt";
    default: return "未知";
    }
}

bool boot_config_tokens_contains(const boot_config_tokens_t *tokens,
                                 const char *value)
{
    size_t index;
    if (tokens == NULL || value == NULL) return false;
    for (index = 0U; index < tokens->count; ++index) {
        if (strcmp(tokens->values[index], value) == 0) return true;
    }
    return false;
}
