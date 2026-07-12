#ifndef SYSTEM_MAINTENANCE_FIDO_STORE_H
#define SYSTEM_MAINTENANCE_FIDO_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * FIDO resident 凭据维护。
 * 存储与 fido_authn / usb_aio_handler 的 storage.c 完全一致：
 * /root/.fido 下每个凭据一个文件，文件名 = credential ID 的 64 位小写 hex，
 * 文件布局（小端）：
 *   u32 magic "FID1" | u8 version=1 | u8 flags | u32 sign_count
 *   priv[32] | rp_id_hash[32]
 *   u8 rp_id_len | rp_id | u8 user_name_len | user_name | u8 user_id_len | user_id
 */

#define FIDO_STORE_DIR "/root/.fido"
#define FIDO_STORE_TAR_PATH "/sd/fido_keys.tar"
#define FIDO_STORE_STAGING_DIR "/tmp/fido_import"

#define FIDO_STORE_CRED_ID_LEN 32U
#define FIDO_STORE_MAX_RP_ID 128U
#define FIDO_STORE_MAX_USER_NAME 128U
#define FIDO_STORE_MAX_USER_ID 64U
#define FIDO_STORE_MAX_FILE_BYTES 1024U

typedef struct {
    uint8_t cred_id[FIDO_STORE_CRED_ID_LEN];
    char file_name[FIDO_STORE_CRED_ID_LEN * 2U + 1U];
    uint32_t sign_count;
    char rp_id[FIDO_STORE_MAX_RP_ID];
    char user_name[FIDO_STORE_MAX_USER_NAME];
    uint8_t user_id[FIDO_STORE_MAX_USER_ID];
    size_t user_id_len;
} fido_store_key_t;

/* 校验文件名是否是合法凭据名（64 位小写 hex） */
bool fido_store_name_is_cred(const char *name);

/* 解析单个凭据文件内容（不含 cred_id，cred_id 来自文件名）。0 成功 */
int fido_store_parse(const uint8_t *data, size_t length, fido_store_key_t *out);

/*
 * 列出目录下全部凭据，按 rp_id / user_name 排序。
 * 返回条数（目录不存在视为 0 条）；-1 出错。*keys 由调用者 free。
 */
int fido_store_list(const char *dir, fido_store_key_t **keys);

/* 删除单个凭据文件。0 成功 */
int fido_store_delete(const char *dir, const fido_store_key_t *key,
                      char *error, size_t error_size);

/* 用 tar（不压缩）把整个凭据目录打包到 tar_path。返回导出的条数；-1 出错 */
int fido_store_export(const char *dir, const char *tar_path,
                      char *error, size_t error_size);

/*
 * 从 tar_path 导入：解包到临时目录，校验每个文件后合并进 dir
 * （同名覆盖）。返回导入条数；-1 出错。
 */
int fido_store_import(const char *dir, const char *tar_path,
                      char *error, size_t error_size);

#endif
