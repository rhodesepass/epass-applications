#define _POSIX_C_SOURCE 200809L

#include "epass_input.h"

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

int epass_input_open_nav(int *fds, int max_fds)
{
    if(!fds || max_fds <= 0) {
        return 0;
    }

    DIR *dir = opendir("/dev/input");
    if(!dir) {
        fprintf(stderr, "epass_input: open /dev/input failed: %s\n", strerror(errno));
        return 0;
    }

    int count = 0;
    struct dirent *ent;
    while((ent = readdir(dir)) != NULL) {
        /* 只认 eventN, 跳过 mice / js* 等 */
        if(strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        if(count >= max_fds) {
            fprintf(stderr, "epass_input: too many devices (max %d), skipping rest\n",
                    max_fds);
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
        fds[count++] = fd;
    }
    closedir(dir);
    return count;
}

void epass_input_close(int *fds, int count)
{
    if(!fds) {
        return;
    }
    for(int i = 0; i < count; i++) {
        if(fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
    }
}
