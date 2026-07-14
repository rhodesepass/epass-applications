#define _POSIX_C_SOURCE 200809L

#include "storage.h"

#include <fcntl.h>
#include <mtd/ubi-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* NAND 上承载 rootfs 的 UBI 分区 (device-tree/cmdline 里的 ubi.mtd=3) */
#define UBI_MTD_INDEX 3
#define SD_BLOCK      "mmcblk0"

bool storage_parse_mtd(const char *proc_mtd, int index,
                       unsigned long long *size_out)
{
    if(!proc_mtd) return false;
    char key[16];
    int klen = snprintf(key, sizeof(key), "mtd%d:", index);

    for(const char *p = proc_mtd; p; p = strchr(p, '\n')) {
        if(*p == '\n') p++;
        if(strncmp(p, key, (size_t)klen) != 0) continue;
        unsigned long long size = 0;
        if(sscanf(p + klen, " %llx", &size) != 1) return false;
        if(size_out) *size_out = size;
        return true;
    }
    return false;
}

bool storage_parse_block_sectors(const char *text, unsigned long long *bytes_out)
{
    if(!text) return false;
    char *end = NULL;
    unsigned long long sectors = strtoull(text, &end, 10);
    if(end == text || sectors == 0) return false;
    if(bytes_out) *bytes_out = sectors * 512ULL;
    return true;
}

void storage_format_size(unsigned long long bytes, char *buf, size_t size)
{
    if(bytes >= 1000ULL * 1000 * 1000) {
        unsigned long long tenths = (bytes + 50000000ULL) / 100000000ULL;
        snprintf(buf, size, "%llu.%llu GB", tenths / 10, tenths % 10);
    } else {
        snprintf(buf, size, "%llu MB", (bytes + 500000ULL) / 1000000ULL);
    }
}

static bool read_text_file(const char *path, char *buf, size_t size)
{
    FILE *fp = fopen(path, "r");
    if(!fp) return false;
    size_t n = fread(buf, 1, size - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return n > 0;
}

static bool read_sysfs_int(const char *path, int *out)
{
    char buf[32];
    if(!read_text_file(path, buf, sizeof(buf))) return false;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if(end == buf) return false;
    *out = (int)v;
    return true;
}

/* 从已挂载的 ubi<dev> 读坏块统计 */
static bool read_ubi_stats(int dev, int *bad, int *reserved)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/ubi/ubi%d/bad_peb_count", dev);
    if(!read_sysfs_int(path, bad)) return false;
    snprintf(path, sizeof(path), "/sys/class/ubi/ubi%d/reserved_for_bad", dev);
    return read_sysfs_int(path, reserved);
}

/* 找 mtd<index> 对应的已挂载 ubi 设备号, 没有返回 -1 */
static int find_attached_ubi(int mtd_index)
{
    for(int dev = 0; dev < 4; dev++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/ubi/ubi%d/mtd_num", dev);
        int num = 0;
        if(read_sysfs_int(path, &num) && num == mtd_index) return dev;
    }
    return -1;
}

/* SD 启动没挂 UBI: 临时挂上读完即卸 */
static bool read_ubi_via_attach(int mtd_index, int *bad, int *reserved)
{
    int fd = open("/dev/ubi_ctrl", O_RDONLY);
    if(fd < 0) return false;

    struct ubi_attach_req req;
    memset(&req, 0, sizeof(req));
    req.ubi_num = UBI_DEV_NUM_AUTO;
    req.mtd_num = mtd_index;
    req.vid_hdr_offset = 0;
    req.max_beb_per1024 = 0; /* 用默认预留策略 */

    /* 成功时返回 0, 内核把分配到的设备号 put_user 写回 req.ubi_num */
    if(ioctl(fd, UBI_IOCATT, &req) < 0 || req.ubi_num < 0) {
        close(fd);
        return false;
    }
    int dev = req.ubi_num;

    bool ok = read_ubi_stats(dev, bad, reserved);

    int32_t d = dev;
    ioctl(fd, UBI_IOCDET, &d);
    close(fd);
    return ok;
}

void storage_probe(storage_info_t *out)
{
    memset(out, 0, sizeof(*out));

    char mtd[1024];
    if(read_text_file("/proc/mtd", mtd, sizeof(mtd)))
        out->nand_present = storage_parse_mtd(mtd, UBI_MTD_INDEX, NULL);

    if(out->nand_present) {
        int dev = find_attached_ubi(UBI_MTD_INDEX);
        if(dev >= 0)
            out->ubi_ok = read_ubi_stats(dev, &out->ubi_bad, &out->ubi_reserved);
        else
            out->ubi_ok = read_ubi_via_attach(UBI_MTD_INDEX,
                                              &out->ubi_bad, &out->ubi_reserved);
    }

    char size[32];
    if(read_text_file("/sys/block/" SD_BLOCK "/size", size, sizeof(size)))
        out->sd_present = storage_parse_block_sectors(size, &out->sd_bytes);
}
