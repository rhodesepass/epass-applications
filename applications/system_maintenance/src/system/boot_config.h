#ifndef SYSTEM_MAINTENANCE_BOOT_CONFIG_H
#define SYSTEM_MAINTENANCE_BOOT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOT_CONFIG_ENV_OFFSET 0U
#define BOOT_CONFIG_ENV_DATA_LENGTH 0x6000U
#define BOOT_CONFIG_ENV_ERASE_LENGTH 0x20000U
#define BOOT_CONFIG_EXPECTED_MTD_SIZE 0x20000U
#define BOOT_CONFIG_EXPECTED_ERASE_SIZE 0x20000U
#define BOOT_CONFIG_EXPECTED_WRITE_SIZE 0x800U

/*
 * SD 启动时 U-Boot 的 load1env_select 从 mmc 0:1（FAT 启动分区）加载
 * env.txt，不读 bootenv MTD；此时环境的读写目标是该文件。
 */
#define BOOT_CONFIG_SD_BOOT_PARTITION "/dev/mmcblk0p1"
#define BOOT_CONFIG_SD_ENV_MOUNTPOINT "/tmp/system_maintenance_boot"
#define BOOT_CONFIG_SD_ENV_FILE BOOT_CONFIG_SD_ENV_MOUNTPOINT "/env.txt"
#define BOOT_CONFIG_SD_ENV_FILE_TMP BOOT_CONFIG_SD_ENV_MOUNTPOINT "/env.tmp"
#define BOOT_CONFIG_MAX_ENTRIES 128U
#define BOOT_CONFIG_MAX_KEY_LENGTH 64U
#define BOOT_CONFIG_MAX_VALUE_LENGTH 1024U
#define BOOT_CONFIG_MAX_TOKENS 32U
#define BOOT_CONFIG_MAX_TOKEN_LENGTH 64U

typedef enum {
    BOOT_CONFIG_BACKEND_NONE = 0,
    BOOT_CONFIG_BACKEND_MTD,     /* NAND 启动：bootenv MTD 分区 */
    BOOT_CONFIG_BACKEND_SD_FILE  /* SD 启动：启动分区里的 env.txt */
} boot_config_backend_t;

typedef struct {
    char values[BOOT_CONFIG_MAX_TOKENS][BOOT_CONFIG_MAX_TOKEN_LENGTH];
    size_t count;
    bool truncated;
} boot_config_tokens_t;

typedef struct {
    char key[BOOT_CONFIG_MAX_KEY_LENGTH];
    char value[BOOT_CONFIG_MAX_VALUE_LENGTH];
} boot_config_entry_t;

typedef struct {
    boot_config_entry_t entries[BOOT_CONFIG_MAX_ENTRIES];
    size_t entry_count;
    char device_rev[BOOT_CONFIG_MAX_VALUE_LENGTH];
    char screen[BOOT_CONFIG_MAX_VALUE_LENGTH];
    boot_config_tokens_t interfaces;
    boot_config_tokens_t extensions;
    bool malformed;
    bool can_write;
    boot_config_backend_t backend;
    char write_reason[256];
    char device_path[128];
} boot_config_t;

typedef struct {
    uint64_t size;
    uint32_t erase_size;
    uint32_t write_size;
    uint32_t type;
    uint32_t flags;
} boot_config_mtd_layout_t;

void boot_config_init(boot_config_t *config);
int boot_config_parse_buffer(const uint8_t *buffer, size_t length,
                             boot_config_t *config);
int boot_config_serialize(const boot_config_t *config, uint8_t *buffer,
                          size_t capacity, size_t *serialized_length);
const char *boot_config_get(const boot_config_t *config, const char *key);
int boot_config_set(boot_config_t *config, const char *key, const char *value);
int boot_config_remove(boot_config_t *config, const char *key);

int boot_config_find_bootenv_mtd(const char *sys_class_mtd,
                                 char *device_path, size_t path_size);
bool boot_config_validate_layout(const boot_config_mtd_layout_t *layout,
                                 char *reason, size_t reason_size);
int boot_config_load(boot_config_t *config, char *error, size_t error_size);
int boot_config_save(boot_config_t *config, char *error, size_t error_size);
bool boot_config_can_write(const boot_config_t *config);
const char *boot_config_write_reason(const boot_config_t *config);
boot_config_backend_t boot_config_backend(const boot_config_t *config);
const char *boot_config_backend_name(boot_config_backend_t backend);
bool boot_config_tokens_contains(const boot_config_tokens_t *tokens,
                                 const char *value);

#endif
