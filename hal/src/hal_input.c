#define _POSIX_C_SOURCE 200809L

#include "hal_input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int key_bit_test(const unsigned long *bits, int bit)
{
    return !!(bits[bit / (sizeof(unsigned long) * 8)] &
              (1UL << (bit % (sizeof(unsigned long) * 8))));
}

/* 设备是否具备我们关心的数字键 (过滤触摸屏等纯 BTN_* 设备) */
static int has_nav_keys(int fd)
{
    unsigned long key_bits[(KEY_MAX + 1 + (sizeof(unsigned long) * 8) - 1) /
                           (sizeof(unsigned long) * 8)];
    memset(key_bits, 0, sizeof(key_bits));
    if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        return 0;
    }
    return key_bit_test(key_bits, KEY_0) ||
           key_bit_test(key_bits, KEY_1) ||
           key_bit_test(key_bits, KEY_2) ||
           key_bit_test(key_bits, KEY_3) ||
           key_bit_test(key_bits, KEY_4);
}

int hal_input_init(hal_input_t *in)
{
    if(!in) return 0;
    for(int i = 0; i < HAL_INPUT_MAX_FDS; i++) in->fds[i] = -1;
    in->fd_count = 0;

    DIR *dir = opendir("/dev/input");
    if(!dir) {
        fprintf(stderr, "hal_input: open /dev/input failed: %s\n", strerror(errno));
        return 0;
    }

    struct dirent *ent;
    while((ent = readdir(dir)) != NULL) {
        /* 只认 eventN, 跳过 mice / js* 等 */
        if(strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        if(in->fd_count >= HAL_INPUT_MAX_FDS) {
            fprintf(stderr, "hal_input: too many devices (max %d), skipping rest\n",
                    HAL_INPUT_MAX_FDS);
            break;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) {
            continue;
        }
        if(!has_nav_keys(fd)) {
            close(fd);
            continue;
        }
        in->fds[in->fd_count++] = fd;
    }
    closedir(dir);
    return in->fd_count;
}

void hal_input_destroy(hal_input_t *in)
{
    if(!in) return;
    for(int i = 0; i < in->fd_count; i++) {
        if(in->fds[i] >= 0) {
            close(in->fds[i]);
            in->fds[i] = -1;
        }
    }
    in->fd_count = 0;
}

static int map_key(unsigned short code)
{
    switch(code) {
    case KEY_1: case KEY_UP:   case KEY_LEFT:  return HAL_KEY_1;
    case KEY_2: case KEY_DOWN: case KEY_RIGHT: return HAL_KEY_2;
    case KEY_3: case KEY_ENTER:                return HAL_KEY_3;
    case KEY_4: case KEY_ESC:  case KEY_BACK:  return HAL_KEY_4;
    default: return -1;
    }
}

bool hal_input_next_event(hal_input_t *in, hal_input_event_t *ev)
{
    struct input_event event;
    if(!in || !ev) return false;
    for(int i = 0; i < in->fd_count; i++) {
        while(read(in->fds[i], &event, sizeof(event)) == sizeof(event)) {
            if(event.type != EV_KEY) continue;
            int key = map_key(event.code);
            if(key < 0) continue;
            ev->key = (hal_key_t)key;
            ev->pressed = event.value != 0;
            ev->repeat = event.value == 2;
            return true;
        }
    }
    return false;
}
