/* wasm 占位: 浏览器无 MTD, 只求编译通过; open("/dev/mtdX") 运行时必失败,
 * 调用方走各自的"后端不可用"错误路径 */
#pragma once
#include <stdint.h>
#include <sys/ioctl.h>

struct mtd_info_user {
    uint8_t type;
    uint32_t flags;
    uint32_t size;
    uint32_t erasesize;
    uint32_t writesize;
    uint32_t oobsize;
    uint64_t padding;
};

struct erase_info_user {
    uint32_t start;
    uint32_t length;
};

#define MEMGETINFO _IOR('M', 1, struct mtd_info_user)
#define MEMERASE   _IOW('M', 2, struct erase_info_user)
#define MTD_NANDFLASH 4
#define MTD_WRITEABLE 0x400

typedef uint64_t loff_t_mtd_compat;
#define MEMGETBADBLOCK _IOW('M', 11, uint64_t)
