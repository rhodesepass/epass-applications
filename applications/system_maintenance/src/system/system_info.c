#define _POSIX_C_SOURCE 200809L

#include "system_info.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void system_info_init(system_info_t *info)
{
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
}

static void copy_value(char *destination, const char *value, size_t length)
{
    /* 值可能带双引号（如 PRETTY_NAME） */
    if (length >= 2U && value[0] == '"' && value[length - 1U] == '"') {
        ++value;
        length -= 2U;
    }
    if (length >= SYSTEM_INFO_FIELD_LENGTH) {
        length = SYSTEM_INFO_FIELD_LENGTH - 1U;
    }
    memcpy(destination, value, length);
    destination[length] = '\0';
}

int system_info_parse_os_release(const char *text, system_info_t *info)
{
    const char *line;
    if (text == NULL || info == NULL) {
        errno = EINVAL;
        return -1;
    }
    system_info_init(info);
    line = text;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        const char *equals;
        size_t line_length;
        if (end == NULL) end = line + strlen(line);
        line_length = (size_t)(end - line);
        equals = memchr(line, '=', line_length);
        if (equals != NULL) {
            size_t key_length = (size_t)(equals - line);
            const char *value = equals + 1;
            size_t value_length = (size_t)(end - value);
            if (key_length == 4U && memcmp(line, "NAME", 4U) == 0) {
                copy_value(info->name, value, value_length);
            } else if (key_length == 7U && memcmp(line, "VERSION", 7U) == 0) {
                copy_value(info->version, value, value_length);
            } else if (key_length == 10U &&
                       memcmp(line, "VERSION_ID", 10U) == 0) {
                copy_value(info->buildroot, value, value_length);
            }
        }
        line = *end == '\0' ? end : end + 1;
    }
    return 0;
}

int system_info_load(system_info_t *info)
{
    FILE *file;
    char buffer[2048];
    size_t length;
    if (info == NULL) {
        errno = EINVAL;
        return -1;
    }
    system_info_init(info);
    file = fopen("/etc/os-release", "r");
    if (file == NULL) file = fopen("/usr/lib/os-release", "r");
    if (file == NULL) return -1;
    length = fread(buffer, 1U, sizeof(buffer) - 1U, file);
    fclose(file);
    buffer[length] = '\0';
    return system_info_parse_os_release(buffer, info);
}
