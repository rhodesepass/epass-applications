#define _POSIX_C_SOURCE 200809L

#include "boot_source.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SD_ROOT_TOKEN "root=/dev/mmcblk0p2"

boot_source_t boot_source_from_cmdline(const char *cmdline)
{
    const char *cursor;

    if (cmdline == NULL) {
        return BOOT_SOURCE_UNKNOWN;
    }
    cursor = cmdline;
    while (*cursor != '\0') {
        const char *end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        end = cursor;
        while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n') {
            ++end;
        }
        if ((size_t)(end - cursor) == sizeof(SD_ROOT_TOKEN) - 1U &&
            strncmp(cursor, SD_ROOT_TOKEN, sizeof(SD_ROOT_TOKEN) - 1U) == 0) {
            return BOOT_SOURCE_SD;
        }
        cursor = end;
    }
    return BOOT_SOURCE_NAND;
}

bool boot_source_path_is_on_sd(const char *path)
{
    return path != NULL &&
           (strcmp(path, "/sd") == 0 || strncmp(path, "/sd/", 4U) == 0);
}

const char *boot_source_name(boot_source_t source)
{
    switch (source) {
    case BOOT_SOURCE_NAND: return "NAND";
    case BOOT_SOURCE_SD: return "SD";
    default: return "UNKNOWN";
    }
}

static char *read_text_file(const char *path)
{
    FILE *file;
    char *buffer;
    size_t used = 0U;
    size_t capacity = 4096U;
    size_t got;

    file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    buffer = malloc(capacity);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    while ((got = fread(buffer + used, 1U, capacity - used - 1U, file)) > 0U) {
        used += got;
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
    }
    buffer[used] = '\0';
    fclose(file);
    return buffer;
}

int boot_source_probe(boot_source_probe_t *result, char *error, size_t error_size)
{
    char *cmdline;
    char exe[4096];
    char cwd[4096];
    ssize_t exe_length;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(result, 0, sizeof(*result));
    cmdline = read_text_file("/proc/cmdline");
    if (cmdline == NULL) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size, "无法读取 /proc/cmdline");
        }
        return -1;
    }
    result->source = boot_source_from_cmdline(cmdline);
    free(cmdline);

    exe_length = readlink("/proc/self/exe", exe, sizeof(exe) - 1U);
    if (exe_length < 0) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size, "无法读取当前程序路径");
        }
        return -1;
    }
    exe[exe_length] = '\0';
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        if (error != NULL && error_size > 0U) {
            (void)snprintf(error, error_size, "无法读取当前工作目录");
        }
        return -1;
    }
    result->executable_on_sd = boot_source_path_is_on_sd(exe);
    result->cwd_on_sd = boot_source_path_is_on_sd(cwd);
    return 0;
}
