#include <assert.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/system/boot_config.h"
#include "../src/system/boot_source.h"
#include "../src/system/fido_store.h"
#include "../src/system/overlay_registry.h"
#include "../src/system/sd_format.h"
#include "../src/system/system_info.h"

static void test_boot_source(void)
{
    /* 判据：cmdline 含 root=/dev/mmcblk0p2 → SD，否则 NAND */
    assert(boot_source_from_cmdline("quiet root=ubi0:rootfs rw") ==
           BOOT_SOURCE_NAND);
    assert(boot_source_from_cmdline("root=/dev/mmcblk0p2 rootwait") ==
           BOOT_SOURCE_SD);
    assert(boot_source_from_cmdline("quiet root=/dev/mmcblk0p2\n") ==
           BOOT_SOURCE_SD);
    assert(boot_source_from_cmdline("root=/dev/mmcblk0p23") ==
           BOOT_SOURCE_NAND);
    assert(boot_source_from_cmdline("root=/dev/sda1") == BOOT_SOURCE_NAND);
    assert(boot_source_from_cmdline("") == BOOT_SOURCE_NAND);
    assert(boot_source_from_cmdline(NULL) == BOOT_SOURCE_UNKNOWN);
    assert(boot_source_path_is_on_sd("/sd/app/maintenance"));
    assert(boot_source_path_is_on_sd("/sd"));
    assert(!boot_source_path_is_on_sd("/sdcard/app"));
}

static void test_boot_config(void)
{
    static const unsigned char data[] =
        "extracmd=ignored\n"
        "device_rev=Epass 0.5\n"
        "screen=360x640\n"
        "interface=i2c0 mystery spi1\n"
        "ext=cardkb unknown_ext\n\0trailing=ignored";
    boot_config_t config;
    boot_config_t reparsed;
    unsigned char oversized[8192];
    unsigned char serialized[BOOT_CONFIG_ENV_DATA_LENGTH];
    size_t serialized_length;
    boot_config_mtd_layout_t layout;
    size_t used = 0U;
    size_t index;

    assert(boot_config_parse_buffer(data, sizeof(data), &config) == 0);
    assert(strcmp(config.device_rev, "Epass 0.5") == 0);
    assert(strcmp(config.screen, "360x640") == 0);
    assert(config.interfaces.count == 3U);
    assert(boot_config_tokens_contains(&config.interfaces, "mystery"));
    assert(boot_config_tokens_contains(&config.extensions, "unknown_ext"));
    assert(!boot_config_can_write(&config));
    /* 未经 load 的配置没有写入后端，save 必须被拒绝 */
    assert(boot_config_backend(&config) == BOOT_CONFIG_BACKEND_NONE);
    assert(boot_config_save(&config, NULL, 0U) != 0);
    assert(strstr(boot_config_backend_name(BOOT_CONFIG_BACKEND_MTD),
                  "MTD") != NULL);
    assert(strstr(boot_config_backend_name(BOOT_CONFIG_BACKEND_SD_FILE),
                  "env.txt") != NULL);
    assert(strcmp(boot_config_get(&config, "extracmd"), "ignored") == 0);
    assert(boot_config_set(&config, "screen", "480x854") == 0);
    assert(boot_config_set(&config, "new_unknown", "keep me") == 0);
    assert(boot_config_serialize(&config, serialized, sizeof(serialized),
                                 &serialized_length) == 0);
    assert(boot_config_parse_buffer(serialized, serialized_length, &reparsed) == 0);
    assert(strcmp(reparsed.screen, "480x854") == 0);
    assert(strcmp(boot_config_get(&reparsed, "new_unknown"), "keep me") == 0);
    assert(strcmp(boot_config_get(&reparsed, "bootargs"), "ignored") == 0);
    assert(boot_config_remove(&reparsed, "new_unknown") == 0);
    assert(boot_config_get(&reparsed, "new_unknown") == NULL);

    used += (size_t)sprintf((char *)oversized + used, "interface=");
    for (index = 0U; index < BOOT_CONFIG_MAX_TOKENS + 2U; ++index) {
        used += (size_t)sprintf((char *)oversized + used, "t%zu ", index);
    }
    oversized[used++] = '\0';
    assert(boot_config_parse_buffer(oversized, used, &config) == 0);
    assert(config.interfaces.count == BOOT_CONFIG_MAX_TOKENS);
    assert(config.interfaces.truncated);
    assert(config.malformed);

    layout.size = BOOT_CONFIG_EXPECTED_MTD_SIZE;
    layout.erase_size = BOOT_CONFIG_EXPECTED_ERASE_SIZE;
    layout.write_size = BOOT_CONFIG_EXPECTED_WRITE_SIZE;
    layout.type = MTD_NANDFLASH;
    layout.flags = MTD_WRITEABLE;
    assert(boot_config_validate_layout(&layout, NULL, 0U));
    assert(BOOT_CONFIG_ENV_OFFSET == 0U);
    assert(BOOT_CONFIG_EXPECTED_MTD_SIZE == BOOT_CONFIG_ENV_ERASE_LENGTH);
    layout.flags = 0U;
    assert(!boot_config_validate_layout(&layout, NULL, 0U));
    layout.flags = MTD_WRITEABLE;
    layout.erase_size = 0x10000U;
    assert(!boot_config_validate_layout(&layout, NULL, 0U));
}

static bool list_has(const char *const *items, size_t count, const char *value)
{
    size_t index;
    for (index = 0U; index < count; ++index) {
        if (strcmp(items[index], value) == 0) return true;
    }
    return false;
}

static void test_registry(void)
{
    const overlay_registry_t *registry = overlay_registry_get();
    const overlay_registry_item_t *item;
    assert(registry->count == 13U);
    item = overlay_registry_find("spi1");
    assert(item != NULL);
    assert(list_has(item->conflicts, item->conflicts_count, "uart2"));
    item = overlay_registry_find("lsm6ds3_pre0.4");
    assert(overlay_registry_available(item, "epass-0.4"));
    assert(!overlay_registry_available(item, "epass-0.5"));
    item = overlay_registry_find("es8311_sound");
    assert(item != NULL);
    assert(item->dependency_mode == OVERLAY_DEPENDENCY_ANY);
    assert(item->requires_count == 2U);
    assert(!overlay_registry_available(item, "unknown"));
    assert(overlay_registry_available(item, "0.6"));
    assert(overlay_registry_find("not_registered") == NULL);
}

static void test_format_safety_helpers(void)
{
    const char *mountinfo =
        "30 22 179:1 / /sd rw - vfat /dev/mmcblk0p1 rw\n"
        "31 30 179:1 /assets /sd/assets\\040copy rw - vfat "
        "/dev/mmcblk0p1 rw\n"
        "32 22 179:0 / /media/raw rw - ext4 /dev/mmcblk0 rw\n"
        "33 22 179:2 / / rw - ext4 /dev/mmcblk0p2 rw\n"
        "34 22 179:3 / /sd rw - vfat /dev/mmcblk0p3 rw\n"
        "40 22 8:1 / /media/other rw - ext4 /dev/sda1 rw\n";
    sd_format_mounts_t mounts;
    sd_format_t format;
    char tool[SD_FORMAT_PATH_MAX];
    const char *script = sd_format_fdisk_script();

    /* 整卡模式：mmcblk0 与其所有分区的挂载点都要卸载 */
    assert(sd_format_mountinfo_collect(mountinfo, SD_FORMAT_MODE_FULL_CARD,
                                       &mounts) == 0);
    assert(mounts.count == 5U);
    assert(strcmp(mounts.paths[0], "/sd") == 0);
    assert(strcmp(mounts.paths[1], "/sd/assets copy") == 0);
    assert(strcmp(mounts.paths[2], "/media/raw") == 0);
    assert(strcmp(mounts.paths[3], "/") == 0);
    assert(strcmp(mounts.paths[4], "/sd") == 0);

    /* SD 启动模式：只允许动 mmcblk0p3，绝不能碰根分区 p2 */
    assert(sd_format_mountinfo_collect(mountinfo, SD_FORMAT_MODE_DATA_PARTITION,
                                       &mounts) == 0);
    assert(mounts.count == 1U);
    assert(strcmp(mounts.paths[0], "/sd") == 0);

    assert(sd_format_mountinfo_collect(mountinfo, SD_FORMAT_MODE_UNKNOWN,
                                       &mounts) != 0);

    assert(sd_format_device_source_is_target_partition("/dev/mmcblk0p1"));
    assert(sd_format_device_source_is_target_partition("/dev/mmcblk0p12"));
    assert(!sd_format_device_source_is_target_partition("/dev/mmcblk1p1"));
    assert(!sd_format_device_source_is_target_partition("/dev/mmcblk0"));

    /* 整卡脚本：单个 FAT32(LBA) 分区，起始 2048 扇区，结束用默认值 */
    assert(strstr(script, "o\n") == script);
    assert(strstr(script, "n\np\n1\n2048\n\n") != NULL);
    assert(strstr(script, "t\nc\n") != NULL);
    assert(strstr(script, "\nw\n") != NULL);
    assert(strstr(script, "\n2\n") == NULL);
    assert(strstr(script, "83") == NULL);

    assert(sd_format_find_tool("sh", "/bin:/usr/bin", tool, sizeof(tool)) == 0);
    assert(tool[0] == '/');

    sd_format_init(&format);
    assert(!sd_format_capable(&format));
    assert(sd_format_mode(&format) == SD_FORMAT_MODE_UNKNOWN);
    assert(strstr(sd_format_capability_reason(&format), "尚未") != NULL);
    assert(sd_format_phase(&format) == SD_FORMAT_IDLE);
    assert(sd_format_cancel(&format));
    assert(sd_format_phase(&format) == SD_FORMAT_CANCELLED);
    sd_format_init(&format);
    format.destructive = true;
    assert(!sd_format_cancel(&format));
}

static void test_system_info(void)
{
    system_info_t info;
    assert(system_info_parse_os_release(
        "NAME=ArkEPass\n"
        "VERSION=a2.7.0-17-ga276145\n"
        "ID=buildroot\n"
        "VERSION_ID=2020.02.7\n"
        "PRETTY_NAME=\"ArkEPassOS a2.7.0-17-ga276145\"\n", &info) == 0);
    assert(strcmp(info.name, "ArkEPass") == 0);
    assert(strcmp(info.version, "a2.7.0-17-ga276145") == 0);
    assert(strcmp(info.buildroot, "2020.02.7") == 0);

    /* 带引号的值去引号；缺字段留空；无换行结尾也能解析 */
    assert(system_info_parse_os_release("VERSION=\"quoted\"", &info) == 0);
    assert(strcmp(info.version, "quoted") == 0);
    assert(info.name[0] == '\0');
    assert(info.buildroot[0] == '\0');
    assert(system_info_parse_os_release(NULL, &info) != 0);
}

static size_t build_cred(uint8_t *out, uint32_t sign_count, const char *rp,
                         const char *user, const uint8_t *uid, size_t uid_len)
{
    size_t used = 0U;
    out[used++] = 0x46; /* "FID1" 小端 */
    out[used++] = 0x49;
    out[used++] = 0x44;
    out[used++] = 0x31;
    out[used++] = 1;
    out[used++] = 0;
    out[used++] = (uint8_t)(sign_count & 0xffU);
    out[used++] = (uint8_t)((sign_count >> 8) & 0xffU);
    out[used++] = (uint8_t)((sign_count >> 16) & 0xffU);
    out[used++] = (uint8_t)((sign_count >> 24) & 0xffU);
    memset(out + used, 0x11, 32U); /* priv */
    used += 32U;
    memset(out + used, 0x22, 32U); /* rp_id_hash */
    used += 32U;
    out[used++] = (uint8_t)strlen(rp);
    memcpy(out + used, rp, strlen(rp));
    used += strlen(rp);
    out[used++] = (uint8_t)strlen(user);
    memcpy(out + used, user, strlen(user));
    used += strlen(user);
    out[used++] = (uint8_t)uid_len;
    memcpy(out + used, uid, uid_len);
    used += uid_len;
    return used;
}

static void write_cred_file(const char *dir, const char *name,
                            const uint8_t *data, size_t length)
{
    char path[512];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1U, length, file) == length);
    fclose(file);
}

static void test_fido_store(void)
{
    uint8_t buffer[FIDO_STORE_MAX_FILE_BYTES];
    uint8_t user_id[3] = {1, 2, 3};
    size_t length;
    fido_store_key_t key;
    fido_store_key_t *keys = NULL;
    char dir[] = "/tmp/fido_store_test_XXXXXX";
    char tar_path[128];
    char error[256];

    /* 文件名校验：64 位小写 hex */
    assert(fido_store_name_is_cred(
        "abcdef0123456789abcdef0123456789"
        "abcdef0123456789abcdef0123456789"));
    assert(!fido_store_name_is_cred("abc"));
    assert(!fido_store_name_is_cred(
        "ABCDEF0123456789ABCDEF0123456789"
        "ABCDEF0123456789ABCDEF0123456789"));
    assert(!fido_store_name_is_cred(
        "abcdef0123456789abcdef0123456789"
        "abcdef0123456789abcdef012345678"));

    /* 解析与拒绝损坏数据 */
    length = build_cred(buffer, 42U, "webauthn.io", "alice", user_id, 3U);
    assert(fido_store_parse(buffer, length, &key) == 0);
    assert(key.sign_count == 42U);
    assert(strcmp(key.rp_id, "webauthn.io") == 0);
    assert(strcmp(key.user_name, "alice") == 0);
    assert(key.user_id_len == 3U && key.user_id[0] == 1);
    assert(fido_store_parse(buffer, length - 2U, &key) != 0);
    buffer[0] ^= 0xffU;
    assert(fido_store_parse(buffer, length, &key) != 0);
    buffer[0] ^= 0xffU;

    /* 列表（排序）、删除、tar 导出/导入闭环 */
    assert(mkdtemp(dir) != NULL);
    snprintf(tar_path, sizeof(tar_path), "%s.tar", dir);
    length = build_cred(buffer, 1U, "zebra.example", "bob", user_id, 3U);
    write_cred_file(dir,
        "1111111111111111111111111111111111111111111111111111111111111111",
        buffer, length);
    length = build_cred(buffer, 2U, "apple.example", "carol", user_id, 3U);
    write_cred_file(dir,
        "2222222222222222222222222222222222222222222222222222222222222222",
        buffer, length);
    write_cred_file(dir, "not_a_cred", buffer, length); /* 应被忽略 */

    assert(fido_store_list(dir, &keys) == 2);
    assert(strcmp(keys[0].rp_id, "apple.example") == 0);
    assert(strcmp(keys[1].rp_id, "zebra.example") == 0);
    assert(keys[0].cred_id[0] == 0x22 && keys[1].cred_id[0] == 0x11);

    assert(fido_store_export(dir, tar_path, error, sizeof(error)) == 2);

    assert(fido_store_delete(dir, &keys[0], error, sizeof(error)) == 0);
    free(keys);
    assert(fido_store_list(dir, &keys) == 1);
    free(keys);

    assert(fido_store_import(dir, tar_path, error, sizeof(error)) == 2);
    assert(fido_store_list(dir, &keys) == 2);
    assert(strcmp(keys[0].rp_id, "apple.example") == 0);
    free(keys);

    /* 不存在的目录 = 0 条；不存在的 tar 报错 */
    assert(fido_store_list("/nonexistent_fido_dir", &keys) == 0);
    assert(keys == NULL);
    assert(fido_store_import(dir, "/nonexistent.tar",
                             error, sizeof(error)) < 0);

    /* 清理 */
    {
        char command[256];
        snprintf(command, sizeof(command), "rm -rf %s %s", dir, tar_path);
        assert(system(command) == 0);
    }
}

int main(void)
{
    test_boot_source();
    test_system_info();
    test_fido_store();
    test_boot_config();
    test_registry();
    test_format_safety_helpers();
    puts("system maintenance backend tests passed");
    return 0;
}
