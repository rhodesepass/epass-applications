#define _POSIX_C_SOURCE 200809L

#include "fido_store.h"
#include "sd_format.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIDO_STORE_MAGIC 0x31444946u /* "FID1" LE */
#define FIDO_STORE_VERSION 1U

static void set_error(char *error, size_t error_size, const char *text)
{
    if (error != NULL && error_size > 0U) {
        (void)snprintf(error, error_size, "%s", text);
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool fido_store_name_is_cred(const char *name)
{
    size_t index;
    if (name == NULL) return false;
    for (index = 0U; index < FIDO_STORE_CRED_ID_LEN * 2U; ++index) {
        if (hex_nibble(name[index]) < 0) return false;
    }
    return name[FIDO_STORE_CRED_ID_LEN * 2U] == '\0';
}

static void decode_cred_id(const char *name, uint8_t out[FIDO_STORE_CRED_ID_LEN])
{
    size_t index;
    for (index = 0U; index < FIDO_STORE_CRED_ID_LEN; ++index) {
        out[index] = (uint8_t)((hex_nibble(name[index * 2U]) << 4) |
                               hex_nibble(name[index * 2U + 1U]));
    }
}

int fido_store_parse(const uint8_t *data, size_t length, fido_store_key_t *out)
{
    size_t cursor = 0U;
    uint32_t magic;
    uint8_t field_length;
    if (data == NULL || out == NULL || length < 10U + 32U + 32U + 3U) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    magic = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
            ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    if (magic != FIDO_STORE_MAGIC || data[4] != FIDO_STORE_VERSION) {
        errno = EINVAL;
        return -1;
    }
    out->sign_count = (uint32_t)data[6] | ((uint32_t)data[7] << 8) |
                      ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);
    cursor = 10U + 32U + 32U; /* 跳过 priv 与 rp_id_hash，维护 UI 不需要 */

    if (cursor >= length) goto malformed;
    field_length = data[cursor++];
    if (field_length >= FIDO_STORE_MAX_RP_ID || cursor + field_length > length)
        goto malformed;
    memcpy(out->rp_id, data + cursor, field_length);
    cursor += field_length;

    if (cursor >= length) goto malformed;
    field_length = data[cursor++];
    if (field_length >= FIDO_STORE_MAX_USER_NAME ||
        cursor + field_length > length)
        goto malformed;
    memcpy(out->user_name, data + cursor, field_length);
    cursor += field_length;

    if (cursor >= length) goto malformed;
    field_length = data[cursor++];
    if (field_length > FIDO_STORE_MAX_USER_ID || cursor + field_length > length)
        goto malformed;
    memcpy(out->user_id, data + cursor, field_length);
    out->user_id_len = field_length;
    return 0;

malformed:
    errno = EINVAL;
    return -1;
}

static int read_small_file(const char *path, uint8_t *buffer, size_t capacity,
                           size_t *length)
{
    FILE *file = fopen(path, "rb");
    size_t used;
    if (file == NULL) return -1;
    used = fread(buffer, 1U, capacity, file);
    if (ferror(file) != 0 || !feof(file)) {
        /* 超过 capacity 的文件不是合法凭据 */
        fclose(file);
        return -1;
    }
    fclose(file);
    *length = used;
    return 0;
}

static int load_key_file(const char *dir, const char *name,
                         fido_store_key_t *out)
{
    char path[512];
    uint8_t buffer[FIDO_STORE_MAX_FILE_BYTES];
    size_t length;
    int written = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    if (read_small_file(path, buffer, sizeof(buffer), &length) != 0) return -1;
    if (fido_store_parse(buffer, length, out) != 0) return -1;
    decode_cred_id(name, out->cred_id);
    memcpy(out->file_name, name, FIDO_STORE_CRED_ID_LEN * 2U + 1U);
    return 0;
}

static int compare_keys(const void *left, const void *right)
{
    const fido_store_key_t *a = left;
    const fido_store_key_t *b = right;
    int order = strcmp(a->rp_id, b->rp_id);
    if (order != 0) return order;
    order = strcmp(a->user_name, b->user_name);
    if (order != 0) return order;
    return strcmp(a->file_name, b->file_name);
}

int fido_store_list(const char *dir, fido_store_key_t **keys)
{
    DIR *directory;
    struct dirent *entry;
    fido_store_key_t *array = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    if (dir == NULL || keys == NULL) {
        errno = EINVAL;
        return -1;
    }
    *keys = NULL;
    directory = opendir(dir);
    if (directory == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        fido_store_key_t key;
        if (!fido_store_name_is_cred(entry->d_name)) continue;
        if (load_key_file(dir, entry->d_name, &key) != 0) continue;
        if (count == capacity) {
            fido_store_key_t *larger;
            capacity = capacity == 0U ? 16U : capacity * 2U;
            larger = realloc(array, capacity * sizeof(*array));
            if (larger == NULL) {
                free(array);
                closedir(directory);
                return -1;
            }
            array = larger;
        }
        array[count++] = key;
    }
    closedir(directory);
    if (count > 0U) {
        qsort(array, count, sizeof(*array), compare_keys);
    }
    *keys = array;
    return (int)count;
}

int fido_store_delete(const char *dir, const fido_store_key_t *key,
                      char *error, size_t error_size)
{
    char path[512];
    int written;
    if (dir == NULL || key == NULL || !fido_store_name_is_cred(key->file_name)) {
        errno = EINVAL;
        set_error(error, error_size, "无效的凭据");
        return -1;
    }
    written = snprintf(path, sizeof(path), "%s/%s", dir, key->file_name);
    if (written < 0 || (size_t)written >= sizeof(path) || unlink(path) != 0) {
        set_error(error, error_size, "删除凭据文件失败");
        return -1;
    }
    return 0;
}

static int run_tar(const char *arg1, const char *arg2, const char *arg3,
                   const char *arg4, const char *arg5,
                   char *error, size_t error_size)
{
    static char tar_path[SD_FORMAT_PATH_MAX];
    char *arguments[7];
    pid_t child;
    int status;
    if (sd_format_find_tool("tar", getenv("PATH"), tar_path,
                            sizeof(tar_path)) != 0) {
        set_error(error, error_size, "系统里找不到 tar");
        return -1;
    }
    arguments[0] = tar_path;
    arguments[1] = (char *)arg1;
    arguments[2] = (char *)arg2;
    arguments[3] = (char *)arg3;
    arguments[4] = (char *)arg4;
    arguments[5] = (char *)arg5;
    arguments[6] = NULL;
    child = fork();
    if (child == 0) {
        execv(tar_path, arguments);
        _exit(127);
    }
    if (child < 0) {
        set_error(error, error_size, "无法启动 tar");
        return -1;
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            set_error(error, error_size, "等待 tar 结束失败");
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(error, error_size, "tar 执行失败");
        return -1;
    }
    return 0;
}

static int split_dir(const char *dir, char *parent, size_t parent_size,
                     const char **base)
{
    const char *slash = strrchr(dir, '/');
    size_t length;
    if (slash == NULL || slash[1] == '\0') return -1;
    length = (size_t)(slash - dir);
    if (length == 0U) length = 1U; /* "/xxx" 的父目录是根 "/" */
    if (length >= parent_size) return -1;
    memcpy(parent, dir, length);
    parent[length] = '\0';
    *base = strrchr(dir, '/') + 1;
    return 0;
}

int fido_store_export(const char *dir, const char *tar_path,
                      char *error, size_t error_size)
{
    fido_store_key_t *keys = NULL;
    int count;
    char parent[512];
    const char *base;
    if (dir == NULL || tar_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    count = fido_store_list(dir, &keys);
    free(keys);
    if (count < 0) {
        set_error(error, error_size, "无法读取凭据目录");
        return -1;
    }
    if (count == 0) {
        set_error(error, error_size, "没有可导出的密钥");
        return -1;
    }
    if (split_dir(dir, parent, sizeof(parent), &base) != 0) {
        set_error(error, error_size, "凭据目录路径无效");
        return -1;
    }
    /* 不压缩，纯 tar */
    if (run_tar("-cf", tar_path, "-C", parent, base, error, error_size) != 0) {
        (void)unlink(tar_path);
        return -1;
    }
    if (access(tar_path, R_OK) != 0) {
        set_error(error, error_size, "导出文件未生成（SD 卡未挂载？）");
        return -1;
    }
    return count;
}

static void remove_tree_shallow(const char *root, int depth)
{
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        char path[768];
        struct stat entry_status;
        int written;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        written = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        if (lstat(path, &entry_status) != 0) continue;
        if (S_ISDIR(entry_status.st_mode)) {
            if (depth > 0) remove_tree_shallow(path, depth - 1);
            (void)rmdir(path);
        } else {
            (void)unlink(path);
        }
    }
    closedir(directory);
}

static int copy_key_file(const char *source_dir, const char *name,
                         const char *target_dir)
{
    char path[768];
    uint8_t buffer[FIDO_STORE_MAX_FILE_BYTES];
    size_t length;
    int fd;
    size_t used = 0U;
    int written = snprintf(path, sizeof(path), "%s/%s", source_dir, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    if (read_small_file(path, buffer, sizeof(buffer), &length) != 0) return -1;
    written = snprintf(path, sizeof(path), "%s/%s", target_dir, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    while (used < length) {
        ssize_t got = write(fd, buffer + used, length - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)got;
    }
    if (fsync(fd) != 0) {
        close(fd);
        return -1;
    }
    return close(fd);
}

/* 从解包结果目录（含一层子目录，如 .fido/）收集合法凭据并拷入 target */
static int import_from_directory(const char *source, const char *target,
                                 int depth)
{
    DIR *directory = opendir(source);
    struct dirent *entry;
    int imported = 0;
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL) {
        char path[768];
        struct stat entry_status;
        int written;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        written = snprintf(path, sizeof(path), "%s/%s", source, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        if (lstat(path, &entry_status) != 0) continue;
        if (S_ISDIR(entry_status.st_mode)) {
            if (depth > 0) {
                imported += import_from_directory(path, target, depth - 1);
            }
            continue;
        }
        if (!S_ISREG(entry_status.st_mode)) continue;
        if (!fido_store_name_is_cred(entry->d_name)) continue;
        {
            fido_store_key_t key;
            if (load_key_file(source, entry->d_name, &key) != 0) continue;
        }
        if (copy_key_file(source, entry->d_name, target) == 0) {
            ++imported;
        }
    }
    closedir(directory);
    return imported;
}

int fido_store_import(const char *dir, const char *tar_path,
                      char *error, size_t error_size)
{
    int imported;
    if (dir == NULL || tar_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (access(tar_path, R_OK) != 0) {
        set_error(error, error_size, "SD 卡上没有 fido_keys.tar");
        return -1;
    }
    remove_tree_shallow(FIDO_STORE_STAGING_DIR, 3);
    (void)rmdir(FIDO_STORE_STAGING_DIR);
    if (mkdir(FIDO_STORE_STAGING_DIR, 0700) != 0) {
        set_error(error, error_size, "无法创建导入临时目录");
        return -1;
    }
    if (run_tar("-xf", tar_path, "-C", FIDO_STORE_STAGING_DIR, NULL,
                error, error_size) != 0) {
        remove_tree_shallow(FIDO_STORE_STAGING_DIR, 3);
        (void)rmdir(FIDO_STORE_STAGING_DIR);
        return -1;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        remove_tree_shallow(FIDO_STORE_STAGING_DIR, 3);
        (void)rmdir(FIDO_STORE_STAGING_DIR);
        set_error(error, error_size, "无法创建凭据目录");
        return -1;
    }
    imported = import_from_directory(FIDO_STORE_STAGING_DIR, dir, 3);
    remove_tree_shallow(FIDO_STORE_STAGING_DIR, 3);
    (void)rmdir(FIDO_STORE_STAGING_DIR);
    if (imported == 0) {
        set_error(error, error_size, "tar 包里没有合法的 FIDO 凭据");
        return -1;
    }
    return imported;
}
