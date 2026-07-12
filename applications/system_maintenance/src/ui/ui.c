#include "ui.h"

#include "../system/boot_config.h"
#include "../system/boot_source.h"
#include "../system/fido_store.h"
#include "../system/overlay_registry.h"
#include "../system/sd_format.h"
#include "../system/system_info.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#ifndef FONT_REGISTRY_DIR
#define FONT_REGISTRY_DIR "/usr/share/fonts/epass"
#endif

#define UI_MAX_LIST_ITEMS 32

typedef enum {
    VIEW_MAIN = 0,
    VIEW_OVERLAYS,
    VIEW_BOOT_CONFIG,
    VIEW_SAVE_CONFIRM,
    VIEW_FORMAT_CONFIRM,
    VIEW_FORMAT_PROGRESS,
    VIEW_REBOOT_CONFIRM,
    VIEW_EXIT_CONFIRM,
    VIEW_FIDO_MENU,
    VIEW_FIDO_LIST,
    VIEW_FIDO_DETAIL,
    VIEW_FIDO_DELETE_CONFIRM,
    VIEW_FIDO_IMPORT_CONFIRM,
    VIEW_MESSAGE
} view_t;

#define FIDO_PAGE_SIZE 8

struct maintenance_ui {
    maintenance_platform_t *platform;
    lv_font_t *font;

    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *status;

    lv_obj_t *list;
    lv_obj_t *list_buttons[UI_MAX_LIST_ITEMS];
    lv_obj_t *list_switches[UI_MAX_LIST_ITEMS];
    int list_count;

    lv_obj_t *msgbox;
    lv_obj_t *progress_bar;
    lv_obj_t *progress_phase;
    lv_obj_t *progress_note;

    lv_style_t style_item;
    lv_style_t style_item_focused;

    boot_config_t *config;
    sd_format_t formatter;
    system_info_t sysinfo;
    boot_source_t boot_source;
    fido_store_key_t *fido_keys;
    int fido_count;
    int fido_index;
    int fido_menu_index;
    view_t view;
    view_t message_return;
    overlay_category_t category;
    int main_index;
    int overlay_index;
    bool config_loaded;
    bool dirty;
    bool should_exit;
    char load_error[256];
};

static const char *const MAIN_ITEMS[] = {
    "接口 Overlay", "扩展 Overlay", "查看启动配置", "保存并应用",
    "FIDO 令牌", "格式化 SD 卡", "重启系统", "退出"
};
#define MAIN_ITEM_COUNT ((int)(sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0])))

static int scaled(const maintenance_ui_t *ui, int value)
{
    int result = value * ui->platform->width / 360;
    return result > 0 ? result : 1;
}

static void set_chrome(maintenance_ui_t *ui, const char *title,
                       const char *status)
{
    lv_label_set_text(ui->title, title);
    lv_label_set_text(ui->status, status);
}

static void close_msgbox(maintenance_ui_t *ui)
{
    if (ui->msgbox != NULL) {
        lv_msgbox_close(ui->msgbox);
        ui->msgbox = NULL;
    }
}

static void clear_body(maintenance_ui_t *ui)
{
    close_msgbox(ui);
    lv_obj_clean(ui->body);
    ui->list = NULL;
    ui->list_count = 0;
    ui->progress_bar = NULL;
    ui->progress_phase = NULL;
    ui->progress_note = NULL;
    memset(ui->list_buttons, 0, sizeof(ui->list_buttons));
    memset(ui->list_switches, 0, sizeof(ui->list_switches));
}

static lv_obj_t *open_msgbox(maintenance_ui_t *ui, const char *title,
                             const char *text, const char *ok_label,
                             const char *cancel_label)
{
    lv_obj_t *box;
    close_msgbox(ui);
    box = lv_msgbox_create(NULL);
    ui->msgbox = box;
    lv_obj_set_style_text_font(box, ui->font, 0);
    lv_obj_set_width(box, ui->platform->width - scaled(ui, 32));
    lv_msgbox_add_title(box, title);
    lv_msgbox_add_text(box, text);
    if (ok_label != NULL) lv_msgbox_add_footer_button(box, ok_label);
    if (cancel_label != NULL) lv_msgbox_add_footer_button(box, cancel_label);
    return box;
}

static lv_obj_t *make_list(maintenance_ui_t *ui)
{
    lv_obj_t *list = lv_list_create(ui->body);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    ui->list = list;
    return list;
}

static lv_obj_t *add_list_item(maintenance_ui_t *ui, const char *text,
                               bool enabled)
{
    lv_obj_t *button;
    if (ui->list_count >= UI_MAX_LIST_ITEMS) return NULL;
    button = lv_list_add_button(ui->list, NULL, text);
    lv_obj_add_style(button, &ui->style_item, 0);
    lv_obj_add_style(button, &ui->style_item_focused, LV_STATE_FOCUS_KEY);
    if (!enabled) lv_obj_add_state(button, LV_STATE_DISABLED);
    ui->list_buttons[ui->list_count] = button;
    ui->list_switches[ui->list_count] = NULL;
    ++ui->list_count;
    return button;
}

static void select_list_item(maintenance_ui_t *ui, int index)
{
    int i;
    for (i = 0; i < ui->list_count; ++i) {
        if (i == index) {
            lv_obj_add_state(ui->list_buttons[i], LV_STATE_FOCUS_KEY);
            lv_obj_scroll_to_view(ui->list_buttons[i], LV_ANIM_OFF);
        } else {
            lv_obj_remove_state(ui->list_buttons[i], LV_STATE_FOCUS_KEY);
        }
    }
}

static bool token_enabled(const maintenance_ui_t *ui,
                          const overlay_registry_item_t *item)
{
    const boot_config_tokens_t *tokens =
        item->category == OVERLAY_CATEGORY_INTERFACE
            ? &ui->config->interfaces : &ui->config->extensions;
    return boot_config_tokens_contains(tokens, item->id);
}

static int set_token(maintenance_ui_t *ui,
                     const overlay_registry_item_t *item, bool enabled)
{
    const char *key = item->category == OVERLAY_CATEGORY_INTERFACE
        ? "interface" : "ext";
    const boot_config_tokens_t *tokens =
        item->category == OVERLAY_CATEGORY_INTERFACE
            ? &ui->config->interfaces : &ui->config->extensions;
    char value[BOOT_CONFIG_MAX_VALUE_LENGTH];
    size_t used = 0U;
    bool found = false;
    size_t i;

    value[0] = '\0';
    for(i = 0U; i < tokens->count; ++i) {
        const char *token = tokens->values[i];
        size_t length;
        if(strcmp(token, item->id) == 0) {
            found = true;
            if(!enabled) continue;
        }
        length = strlen(token);
        if(used + length + (used ? 1U : 0U) >= sizeof(value)) return -1;
        if(used) value[used++] = ' ';
        memcpy(value + used, token, length);
        used += length;
        value[used] = '\0';
    }
    if(enabled && !found) {
        size_t length = strlen(item->id);
        if(used + length + (used ? 1U : 0U) >= sizeof(value)) return -1;
        if(used) value[used++] = ' ';
        memcpy(value + used, item->id, length + 1U);
    }
    return boot_config_set(ui->config, key, value);
}

static int enable_overlay(maintenance_ui_t *ui,
                          const overlay_registry_item_t *item, int depth)
{
    size_t i;
    if(depth > 8 || item == NULL ||
       !overlay_registry_available(item, ui->config->device_rev)) return -1;

    for(i = 0U; i < item->conflicts_count; ++i) {
        const overlay_registry_item_t *conflict =
            overlay_registry_find(item->conflicts[i]);
        if(conflict && set_token(ui, conflict, false) != 0) return -1;
    }
    if(item->requires_count > 0U) {
        size_t begin = 0U, end = item->requires_count;
        if(item->dependency_mode == OVERLAY_DEPENDENCY_ANY) {
            for(i = 0U; i < item->requires_count; ++i) {
                const overlay_registry_item_t *required =
                    overlay_registry_find(item->requires[i]);
                if(required && token_enabled(ui, required)) break;
            }
            if(i < item->requires_count) begin = end;
            else end = 1U;
        }
        for(i = begin; i < end; ++i) {
            const overlay_registry_item_t *required =
                overlay_registry_find(item->requires[i]);
            if(enable_overlay(ui, required, depth + 1) != 0) return -1;
        }
    }
    return set_token(ui, item, true);
}

static size_t category_count(overlay_category_t category)
{
    const overlay_registry_t *registry = overlay_registry_get();
    size_t count = 0U, i;
    for(i = 0U; i < registry->count; ++i)
        if(registry->items[i].category == category) ++count;
    return count;
}

static const overlay_registry_item_t *category_item(
    overlay_category_t category, size_t wanted)
{
    const overlay_registry_t *registry = overlay_registry_get();
    size_t seen = 0U, i;
    for(i = 0U; i < registry->count; ++i) {
        if(registry->items[i].category != category) continue;
        if(seen++ == wanted) return &registry->items[i];
    }
    return NULL;
}

static void render_main(maintenance_ui_t *ui)
{
    int i;
    char info_text[512];
    lv_obj_t *info;
    clear_body(ui);

    snprintf(info_text, sizeof(info_text),
             "系统版本：%s\nBuildroot：%s\n硬件版本：%s\n启动方式：%s",
             ui->sysinfo.version[0] ? ui->sysinfo.version : "未知",
             ui->sysinfo.buildroot[0] ? ui->sysinfo.buildroot : "未知",
             ui->config->device_rev[0] ? ui->config->device_rev : "未知",
             ui->boot_source == BOOT_SOURCE_NAND ? "NAND"
                 : ui->boot_source == BOOT_SOURCE_SD ? "SD 卡" : "未知");
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui->body, scaled(ui, 8), 0);
    info = lv_label_create(ui->body);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(info, lv_color_hex(0x9BB0C1), 0);
    lv_label_set_text(info, info_text);

    make_list(ui);
    lv_obj_set_height(ui->list, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui->list, 1);
    for (i = 0; i < MAIN_ITEM_COUNT; ++i) {
        char text[192];
        const char *suffix = "";
        bool enabled = true;
        if (i == 3) {
            if (!ui->config_loaded || !boot_config_can_write(ui->config)) {
                suffix = "（不可用）";
                enabled = false;
            } else if (ui->dirty) {
                suffix = "（有更改）";
            }
        }
        if (i == 5 && !sd_format_capable(&ui->formatter)) {
            suffix = "（不可用）";
            enabled = false;
        }
        snprintf(text, sizeof(text), "%s%s", MAIN_ITEMS[i], suffix);
        add_list_item(ui, text, enabled);
    }
    select_list_item(ui, ui->main_index);
    set_chrome(ui, "系统维护", "KEY1/2 选择  KEY3 确认  KEY4 退出");
    ui->view = VIEW_MAIN;
}

static void render_overlays(maintenance_ui_t *ui)
{
    const char *title = ui->category == OVERLAY_CATEGORY_INTERFACE
        ? "接口 Overlay" : "扩展 Overlay";
    size_t count = category_count(ui->category), i;
    clear_body(ui);
    make_list(ui);
    for (i = 0U; i < count; ++i) {
        const overlay_registry_item_t *item = category_item(ui->category, i);
        bool available =
            overlay_registry_available(item, ui->config->device_rev);
        char text[192];
        lv_obj_t *button;
        lv_obj_t *toggle;
        snprintf(text, sizeof(text), "%s%s", item->name_zh,
                 available ? "" : "（硬件不支持）");
        button = add_list_item(ui, text, available);
        if (button == NULL) break;
        toggle = lv_switch_create(button);
        lv_obj_set_size(toggle, scaled(ui, 44), scaled(ui, 24));
        if (token_enabled(ui, item)) lv_obj_add_state(toggle, LV_STATE_CHECKED);
        if (!available) lv_obj_add_state(toggle, LV_STATE_DISABLED);
        ui->list_switches[ui->list_count - 1] = toggle;
    }
    select_list_item(ui, ui->overlay_index);
    set_chrome(ui, title,
               boot_config_can_write(ui->config)
                   ? "KEY3 切换  KEY4 返回；保存后重启生效"
                   : boot_config_write_reason(ui->config));
    ui->view = VIEW_OVERLAYS;
}

static void render_boot_config(maintenance_ui_t *ui)
{
    char text[2300];
    const char *bootargs = boot_config_get(ui->config, "bootargs");
    const char *interfaces = boot_config_get(ui->config, "interface");
    const char *extensions = boot_config_get(ui->config, "ext");
    lv_obj_t *label;
    snprintf(text, sizeof(text),
             "设备版本：%s\n屏幕：%s\n接口：%s\n扩展：%s\n\n"
             "写入后端：%s（%s）\n%s\n\nbootargs:\n%s",
             ui->config->device_rev[0] ? ui->config->device_rev : "未知",
             ui->config->screen[0] ? ui->config->screen : "未知",
             interfaces ? interfaces : "", extensions ? extensions : "",
             boot_config_backend_name(boot_config_backend(ui->config)),
             boot_config_can_write(ui->config) ? "可用" : "不可用",
             boot_config_write_reason(ui->config), bootargs ? bootargs : "");
    clear_body(ui);
    label = lv_label_create(ui->body);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    set_chrome(ui, "启动配置", "KEY1/2 滚动  KEY4 返回");
    ui->view = VIEW_BOOT_CONFIG;
}

static void show_message(maintenance_ui_t *ui, const char *title,
                         const char *message, view_t return_to)
{
    ui->message_return = return_to;
    open_msgbox(ui, title, message, "返回 (KEY3/4)", NULL);
    lv_label_set_text(ui->status, "KEY3/4 返回");
    ui->view = VIEW_MESSAGE;
}

static const char *format_phase_name(sd_format_phase_t phase)
{
    switch(phase) {
    case SD_FORMAT_PREFLIGHT: return "安全检查";
    case SD_FORMAT_UNMOUNTING: return "卸载现有挂载点";
    case SD_FORMAT_PARTITIONING: return "重建分区表";
    case SD_FORMAT_WAITING_PARTITION: return "等待分区设备";
    case SD_FORMAT_CREATING_FILESYSTEM: return "创建 FAT 文件系统";
    case SD_FORMAT_MOUNTING: return "挂载数据分区";
    case SD_FORMAT_POPULATING: return "初始化目录";
    case SD_FORMAT_REFRESHING_MTP: return "刷新 MTP";
    case SD_FORMAT_COMPLETE: return "完成";
    case SD_FORMAT_FAILED: return "失败";
    case SD_FORMAT_CANCELLED: return "已取消";
    default: return "准备";
    }
}

static void render_format_progress(maintenance_ui_t *ui)
{
    lv_obj_t *bar;
    clear_body(ui);
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui->body, scaled(ui, 18), 0);

    ui->progress_phase = lv_label_create(ui->body);
    lv_obj_set_width(ui->progress_phase, lv_pct(100));
    lv_obj_set_style_text_align(ui->progress_phase, LV_TEXT_ALIGN_CENTER, 0);

    bar = lv_bar_create(ui->body);
    lv_obj_set_size(bar, lv_pct(90), scaled(ui, 18));
    lv_bar_set_range(bar, 0, 100);
    ui->progress_bar = bar;

    ui->progress_note = lv_label_create(ui->body);
    lv_obj_set_width(ui->progress_note, lv_pct(100));
    lv_label_set_long_mode(ui->progress_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(ui->progress_note, LV_TEXT_ALIGN_CENTER, 0);

    set_chrome(ui, "格式化 SD 卡", "");
    ui->view = VIEW_FORMAT_PROGRESS;
}

static void update_format_progress(maintenance_ui_t *ui)
{
    sd_format_phase_t phase = sd_format_phase(&ui->formatter);
    if (ui->progress_bar == NULL) return;
    lv_bar_set_value(ui->progress_bar,
                     (int32_t)sd_format_progress(&ui->formatter), LV_ANIM_OFF);
    lv_label_set_text_fmt(ui->progress_phase, "%s（%u%%）",
                          format_phase_name(phase),
                          sd_format_progress(&ui->formatter));
    lv_label_set_text(ui->progress_note,
                      phase == SD_FORMAT_FAILED
                          ? sd_format_error(&ui->formatter)
                          : (ui->formatter.destructive
                                 ? "正在写入分区，请勿关机或拔卡。"
                                 : "正在准备。"));
    lv_label_set_text(ui->status,
                      sd_format_finished(&ui->formatter)
                          ? "KEY3/4 返回"
                          : (ui->formatter.destructive
                                 ? "操作不可取消" : "KEY4 取消"));
}

static void fido_reload(maintenance_ui_t *ui)
{
    free(ui->fido_keys);
    ui->fido_keys = NULL;
    ui->fido_count = fido_store_list(FIDO_STORE_DIR, &ui->fido_keys);
    if (ui->fido_count < 0) ui->fido_count = 0;
    if (ui->fido_index >= ui->fido_count) ui->fido_index = ui->fido_count - 1;
    if (ui->fido_index < 0) ui->fido_index = 0;
}

static const char *fido_rp_display(const fido_store_key_t *key)
{
    return key->rp_id[0] != '\0' ? key->rp_id : "U2F 凭据";
}

static void render_fido_menu(maintenance_ui_t *ui)
{
    char text[192];
    clear_body(ui);
    make_list(ui);
    snprintf(text, sizeof(text), "查看 / 删除密钥（共 %d 个）", ui->fido_count);
    add_list_item(ui, text, true);
    add_list_item(ui, "导出密钥到 SD 卡", true);
    add_list_item(ui, "从 SD 卡导入密钥", true);
    select_list_item(ui, ui->fido_menu_index);
    set_chrome(ui, "FIDO 令牌",
               "KEY1/2 选择  KEY3 确认  KEY4 返回");
    ui->view = VIEW_FIDO_MENU;
}

static void render_fido_list(maintenance_ui_t *ui)
{
    int page, page_count, begin, end, i;
    char status[96];
    clear_body(ui);
    if (ui->fido_count == 0) {
        lv_obj_t *label = lv_label_create(ui->body);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, "没有 FIDO 密钥。\n\n"
                                 "在 FIDO 模式下注册的通行密钥会保存在\n"
                                 "/root/.fido 目录。");
        set_chrome(ui, "FIDO 密钥", "KEY4 返回");
        ui->view = VIEW_FIDO_LIST;
        return;
    }
    page_count = (ui->fido_count + FIDO_PAGE_SIZE - 1) / FIDO_PAGE_SIZE;
    page = ui->fido_index / FIDO_PAGE_SIZE;
    begin = page * FIDO_PAGE_SIZE;
    end = begin + FIDO_PAGE_SIZE;
    if (end > ui->fido_count) end = ui->fido_count;
    make_list(ui);
    for (i = begin; i < end; ++i) {
        const fido_store_key_t *key = &ui->fido_keys[i];
        char text[192];
        if (key->user_name[0] != '\0') {
            snprintf(text, sizeof(text), "%d. %s（%s）",
                     i + 1, fido_rp_display(key), key->user_name);
        } else {
            snprintf(text, sizeof(text), "%d. %s", i + 1, fido_rp_display(key));
        }
        add_list_item(ui, text, true);
    }
    select_list_item(ui, ui->fido_index - begin);
    snprintf(status, sizeof(status),
             "第 %d/%d 页  KEY3 详情  KEY4 返回", page + 1, page_count);
    set_chrome(ui, "FIDO 密钥", status);
    ui->view = VIEW_FIDO_LIST;
}

static void append_hex(char *destination, size_t capacity,
                       const uint8_t *data, size_t length)
{
    size_t used = strlen(destination);
    size_t index;
    for (index = 0U; index < length && used + 3U < capacity; ++index) {
        (void)snprintf(destination + used, capacity - used, "%02x",
                       data[index]);
        used += 2U;
    }
}

static void render_fido_detail(maintenance_ui_t *ui)
{
    const fido_store_key_t *key;
    char text[1024];
    lv_obj_t *label;
    if (ui->fido_index < 0 || ui->fido_index >= ui->fido_count) {
        render_fido_list(ui);
        return;
    }
    key = &ui->fido_keys[ui->fido_index];
    snprintf(text, sizeof(text),
             "站点 (rpId)：%s\n用户：%s\n签名计数：%u\n\n用户 ID：\n",
             fido_rp_display(key),
             key->user_name[0] != '\0' ? key->user_name : "（无）",
             (unsigned)key->sign_count);
    if (key->user_id_len > 0U) {
        append_hex(text, sizeof(text), key->user_id, key->user_id_len);
    } else {
        strncat(text, "（无）", sizeof(text) - strlen(text) - 1U);
    }
    strncat(text, "\n\n凭据 ID：\n", sizeof(text) - strlen(text) - 1U);
    append_hex(text, sizeof(text), key->cred_id, FIDO_STORE_CRED_ID_LEN);
    clear_body(ui);
    label = lv_label_create(ui->body);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    set_chrome(ui, "密钥详情", "KEY1/2 滚动  KEY3 删除  KEY4 返回");
    ui->view = VIEW_FIDO_DETAIL;
}

static void return_from_modal(maintenance_ui_t *ui)
{
    close_msgbox(ui);
    if (ui->message_return == VIEW_OVERLAYS) render_overlays(ui);
    else if (ui->message_return == VIEW_FIDO_MENU) render_fido_menu(ui);
    else if (ui->message_return == VIEW_FIDO_LIST) render_fido_list(ui);
    else render_main(ui);
}

static void open_confirm(maintenance_ui_t *ui, const char *title,
                         const char *text, view_t view)
{
    open_msgbox(ui, title, text, "确认 (KEY3)", "取消 (KEY4)");
    lv_label_set_text(ui->status, "KEY3 确认  KEY4 取消");
    ui->view = view;
}

static void activate_main(maintenance_ui_t *ui)
{
    switch(ui->main_index) {
    case 0:
    case 1:
        if(!ui->config_loaded) {
            show_message(ui, "无法读取配置", ui->load_error, VIEW_MAIN);
            return;
        }
        ui->category = ui->main_index == 0
            ? OVERLAY_CATEGORY_INTERFACE : OVERLAY_CATEGORY_EXTENSION;
        ui->overlay_index = 0;
        render_overlays(ui);
        break;
    case 2:
        if(ui->config_loaded) render_boot_config(ui);
        else show_message(ui, "无法读取配置", ui->load_error, VIEW_MAIN);
        break;
    case 3:
        if(!ui->config_loaded || !boot_config_can_write(ui->config)) {
            show_message(ui, "无法保存",
                         ui->config_loaded ? boot_config_write_reason(ui->config)
                                           : ui->load_error, VIEW_MAIN);
        } else if(!ui->dirty) {
            show_message(ui, "无需保存", "启动配置没有更改。", VIEW_MAIN);
        } else {
            open_confirm(ui, "确认保存",
                         boot_config_backend(ui->config) ==
                                 BOOT_CONFIG_BACKEND_SD_FILE
                             ? "将改写 SD 启动分区中的 env.txt。\n\n"
                               "保存期间请勿关机，完成后必须重启才能生效。"
                             : "将擦除并重写 bootenv PEB。\n\n"
                               "保存期间请勿关机，完成后必须重启才能生效。",
                         VIEW_SAVE_CONFIRM);
        }
        break;
    case 4:
        ui->fido_menu_index = 0;
        ui->fido_index = 0;
        fido_reload(ui);
        render_fido_menu(ui);
        break;
    case 5:
        if(!sd_format_capable(&ui->formatter)) {
            show_message(ui, "无法格式化",
                         sd_format_capability_reason(&ui->formatter), VIEW_MAIN);
        } else if(sd_format_mode(&ui->formatter) == SD_FORMAT_MODE_FULL_CARD) {
            open_confirm(ui, "危险操作",
                         "当前为 NAND 启动。\n\n"
                         "将删除 SD 卡全部数据，并把整张卡\n"
                         "重建为单个 FAT32 分区。",
                         VIEW_FORMAT_CONFIRM);
        } else {
            open_confirm(ui, "危险操作",
                         "当前为 SD 启动。\n\n"
                         "将清空数据分区 mmcblk0p3\n"
                         "并重新格式化为 FAT。\n"
                         "系统分区不受影响。",
                         VIEW_FORMAT_CONFIRM);
        }
        break;
    case 6:
        open_confirm(ui, "确认重启", "确定立即重启系统？", VIEW_REBOOT_CONFIRM);
        break;
    default:
        open_confirm(ui, "确认退出",
                     ui->dirty ? "尚有未保存的更改，确定退出？"
                               : "确定退出系统维护？",
                     VIEW_EXIT_CONFIRM);
        break;
    }
}

maintenance_ui_t *maintenance_ui_create(maintenance_platform_t *platform)
{
    maintenance_ui_t *ui = calloc(1U, sizeof(*ui));
    char font_path[512];
    int margin, status_h, title_h;
    if(!ui) return NULL;
    ui->config = calloc(1U, sizeof(*ui->config));
    if(!ui->config) {
        free(ui);
        return NULL;
    }
    ui->platform = platform;
    boot_config_init(ui->config);
    ui->config_loaded =
        boot_config_load(ui->config, ui->load_error, sizeof(ui->load_error)) == 0;
    sd_format_init(&ui->formatter);
    (void)sd_format_preflight(&ui->formatter);
    (void)system_info_load(&ui->sysinfo);
    {
        boot_source_probe_t probe;
        ui->boot_source = boot_source_probe(&probe, NULL, 0U) == 0
            ? probe.source : BOOT_SOURCE_UNKNOWN;
    }

    snprintf(font_path, sizeof(font_path), "%s/SourceHanSansSC-Regular.otf",
             FONT_REGISTRY_DIR);
    if(access(font_path, R_OK) != 0)
        snprintf(font_path, sizeof(font_path),
                 "/usr/share/fonts/epass/SourceHanSansSC-Regular.otf");
    ui->font = lv_freetype_font_create(font_path,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)scaled(ui, 20),
        LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!ui->font) {
        free(ui->config);
        free(ui);
        return NULL;
    }

    lv_style_init(&ui->style_item);
    lv_style_set_bg_color(&ui->style_item, lv_color_hex(0x18242E));
    lv_style_set_bg_opa(&ui->style_item, LV_OPA_COVER);
    lv_style_set_text_color(&ui->style_item, lv_color_hex(0xE0E0E0));
    lv_style_set_radius(&ui->style_item, scaled(ui, 6));
    lv_style_set_pad_ver(&ui->style_item, scaled(ui, 10));
    lv_style_init(&ui->style_item_focused);
    lv_style_set_bg_color(&ui->style_item_focused, lv_color_hex(0x2F6FED));
    lv_style_set_text_color(&ui->style_item_focused, lv_color_hex(0xFFFFFF));

    margin = scaled(ui, 12);
    status_h = scaled(ui, 42);
    title_h = scaled(ui, 48);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(lv_screen_active(), ui->font, 0);
    lv_obj_set_style_text_font(lv_layer_top(), ui->font, 0);
    lv_obj_set_style_text_color(lv_layer_top(), lv_color_hex(0x202020), 0);

    ui->title = lv_label_create(lv_screen_active());
    lv_obj_set_size(ui->title, platform->width - margin * 2, title_h);
    lv_obj_align(ui->title, LV_ALIGN_TOP_MID, 0, margin);
    lv_obj_set_style_text_color(ui->title, lv_color_hex(0xF2F2F2), 0);

    ui->body = lv_obj_create(lv_screen_active());
    lv_obj_set_size(ui->body, platform->width - margin * 2,
                    platform->height - title_h - status_h - margin * 3);
    lv_obj_align(ui->body, LV_ALIGN_TOP_MID, 0, margin + title_h);
    lv_obj_set_style_bg_opa(ui->body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->body, 0, 0);
    lv_obj_set_style_pad_all(ui->body, 0, 0);
    lv_obj_set_style_text_color(ui->body, lv_color_hex(0xE0E0E0), 0);

    ui->status = lv_label_create(lv_screen_active());
    lv_obj_set_size(ui->status, platform->width, status_h);
    lv_obj_align(ui->status, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(ui->status, scaled(ui, 5), 0);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(ui->status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(ui->status, lv_color_hex(0x263746), 0);
    lv_obj_set_style_bg_opa(ui->status, LV_OPA_COVER, 0);

    render_main(ui);
    return ui;
}

void maintenance_ui_handle_key(maintenance_ui_t *ui, maintenance_key_t key)
{
    if(!ui || key == MAINTENANCE_KEY_NONE) return;
    if(ui->view == VIEW_MAIN) {
        if(key == MAINTENANCE_KEY_PREV) {
            ui->main_index =
                (ui->main_index + MAIN_ITEM_COUNT - 1) % MAIN_ITEM_COUNT;
            select_list_item(ui, ui->main_index);
        } else if(key == MAINTENANCE_KEY_NEXT) {
            ui->main_index = (ui->main_index + 1) % MAIN_ITEM_COUNT;
            select_list_item(ui, ui->main_index);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            activate_main(ui);
        } else if(key == MAINTENANCE_KEY_BACK) {
            ui->main_index = MAIN_ITEM_COUNT - 1;
            select_list_item(ui, ui->main_index);
            activate_main(ui);
        }
    } else if(ui->view == VIEW_OVERLAYS) {
        int count = (int)category_count(ui->category);
        if(key == MAINTENANCE_KEY_PREV) {
            ui->overlay_index = (ui->overlay_index + count - 1) % count;
            select_list_item(ui, ui->overlay_index);
        } else if(key == MAINTENANCE_KEY_NEXT) {
            ui->overlay_index = (ui->overlay_index + 1) % count;
            select_list_item(ui, ui->overlay_index);
        } else if(key == MAINTENANCE_KEY_BACK) {
            render_main(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            const overlay_registry_item_t *item =
                category_item(ui->category, (size_t)ui->overlay_index);
            if(!boot_config_can_write(ui->config)) {
                show_message(ui, "无法修改",
                             boot_config_write_reason(ui->config), VIEW_OVERLAYS);
                return;
            }
            if(!overlay_registry_available(item, ui->config->device_rev)) {
                show_message(ui, "硬件不支持", item->description_zh, VIEW_OVERLAYS);
                return;
            }
            if((token_enabled(ui, item) ? set_token(ui, item, false)
                                       : enable_overlay(ui, item, 0)) != 0) {
                show_message(ui, "修改失败", "配置项过多或依赖无法满足。",
                             VIEW_OVERLAYS);
                return;
            }
            ui->dirty = true;
            /* 级联启用/冲突禁用可能改动多项，整页重建最保险 */
            render_overlays(ui);
        }
    } else if(ui->view == VIEW_BOOT_CONFIG) {
        if(key == MAINTENANCE_KEY_PREV)
            lv_obj_scroll_by_bounded(ui->body, 0, scaled(ui, 120), LV_ANIM_OFF);
        else if(key == MAINTENANCE_KEY_NEXT)
            lv_obj_scroll_by_bounded(ui->body, 0, -scaled(ui, 120), LV_ANIM_OFF);
        else if(key == MAINTENANCE_KEY_BACK || key == MAINTENANCE_KEY_ENTER)
            render_main(ui);
    } else if(ui->view == VIEW_SAVE_CONFIRM) {
        if(key == MAINTENANCE_KEY_BACK) {
            close_msgbox(ui);
            render_main(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            char error[256];
            if(boot_config_save(ui->config, error, sizeof(error)) == 0) {
                ui->dirty = false;
                show_message(ui, "保存成功",
                             "启动配置已写入并完成读回校验。\n请重启后生效。",
                             VIEW_MAIN);
            } else {
                show_message(ui, "保存失败", error, VIEW_MAIN);
            }
        }
    } else if(ui->view == VIEW_FORMAT_CONFIRM) {
        if(key == MAINTENANCE_KEY_BACK) {
            close_msgbox(ui);
            render_main(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            close_msgbox(ui);
            if(sd_format_start(&ui->formatter) != 0) {
                show_message(ui, "无法格式化", sd_format_error(&ui->formatter),
                             VIEW_MAIN);
            } else {
                render_format_progress(ui);
                update_format_progress(ui);
            }
        }
    } else if(ui->view == VIEW_FORMAT_PROGRESS) {
        if(sd_format_finished(&ui->formatter)) {
            if(key == MAINTENANCE_KEY_BACK || key == MAINTENANCE_KEY_ENTER) {
                sd_format_init(&ui->formatter);
                (void)sd_format_preflight(&ui->formatter);
                render_main(ui);
            }
        } else if(key == MAINTENANCE_KEY_BACK && sd_format_cancel(&ui->formatter)) {
            update_format_progress(ui);
        }
    } else if(ui->view == VIEW_REBOOT_CONFIRM) {
        if(key == MAINTENANCE_KEY_BACK) {
            close_msgbox(ui);
            render_main(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            sync();
            if(reboot(RB_AUTOBOOT) != 0)
                show_message(ui, "重启失败", strerror(errno), VIEW_MAIN);
        }
    } else if(ui->view == VIEW_EXIT_CONFIRM) {
        if(key == MAINTENANCE_KEY_ENTER) {
            ui->should_exit = true;
        } else if(key == MAINTENANCE_KEY_BACK) {
            close_msgbox(ui);
            render_main(ui);
        }
    } else if(ui->view == VIEW_FIDO_MENU) {
        if(key == MAINTENANCE_KEY_PREV) {
            ui->fido_menu_index = (ui->fido_menu_index + 2) % 3;
            select_list_item(ui, ui->fido_menu_index);
        } else if(key == MAINTENANCE_KEY_NEXT) {
            ui->fido_menu_index = (ui->fido_menu_index + 1) % 3;
            select_list_item(ui, ui->fido_menu_index);
        } else if(key == MAINTENANCE_KEY_BACK) {
            render_main(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            if(ui->fido_menu_index == 0) {
                fido_reload(ui);
                ui->fido_index = 0;
                render_fido_list(ui);
            } else if(ui->fido_menu_index == 1) {
                char error[256];
                int exported = fido_store_export(FIDO_STORE_DIR,
                                                 FIDO_STORE_TAR_PATH,
                                                 error, sizeof(error));
                if(exported < 0) {
                    show_message(ui, "导出失败", error, VIEW_FIDO_MENU);
                } else {
                    char text[192];
                    snprintf(text, sizeof(text),
                             "已导出 %d 个密钥到\n" FIDO_STORE_TAR_PATH
                             "\n\ntar 归档未压缩，也未加密，\n请妥善保管。",
                             exported);
                    show_message(ui, "导出成功", text, VIEW_FIDO_MENU);
                }
            } else {
                open_confirm(ui, "确认导入",
                             "将从 " FIDO_STORE_TAR_PATH " 导入密钥。\n\n"
                             "与现有密钥同名的条目会被覆盖。",
                             VIEW_FIDO_IMPORT_CONFIRM);
            }
        }
    } else if(ui->view == VIEW_FIDO_LIST) {
        if(ui->fido_count == 0) {
            if(key == MAINTENANCE_KEY_BACK || key == MAINTENANCE_KEY_ENTER)
                render_fido_menu(ui);
        } else if(key == MAINTENANCE_KEY_PREV || key == MAINTENANCE_KEY_NEXT) {
            int page_before = ui->fido_index / FIDO_PAGE_SIZE;
            if(key == MAINTENANCE_KEY_PREV)
                ui->fido_index = (ui->fido_index + ui->fido_count - 1) %
                                 ui->fido_count;
            else
                ui->fido_index = (ui->fido_index + 1) % ui->fido_count;
            if(ui->fido_index / FIDO_PAGE_SIZE != page_before)
                render_fido_list(ui);
            else
                select_list_item(ui,
                                 ui->fido_index % FIDO_PAGE_SIZE);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            render_fido_detail(ui);
        } else if(key == MAINTENANCE_KEY_BACK) {
            render_fido_menu(ui);
        }
    } else if(ui->view == VIEW_FIDO_DETAIL) {
        if(key == MAINTENANCE_KEY_PREV)
            lv_obj_scroll_by_bounded(ui->body, 0, scaled(ui, 120), LV_ANIM_OFF);
        else if(key == MAINTENANCE_KEY_NEXT)
            lv_obj_scroll_by_bounded(ui->body, 0, -scaled(ui, 120), LV_ANIM_OFF);
        else if(key == MAINTENANCE_KEY_ENTER)
            open_confirm(ui, "确认删除",
                         "将永久删除该 FIDO 密钥，\n对应网站的通行密钥登录会失效。",
                         VIEW_FIDO_DELETE_CONFIRM);
        else if(key == MAINTENANCE_KEY_BACK)
            render_fido_list(ui);
    } else if(ui->view == VIEW_FIDO_DELETE_CONFIRM) {
        if(key == MAINTENANCE_KEY_BACK) {
            close_msgbox(ui);
            render_fido_detail(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            char error[256];
            close_msgbox(ui);
            if(ui->fido_index < ui->fido_count &&
               fido_store_delete(FIDO_STORE_DIR,
                                 &ui->fido_keys[ui->fido_index],
                                 error, sizeof(error)) == 0) {
                fido_reload(ui);
                show_message(ui, "已删除", "该密钥已从设备移除。",
                             VIEW_FIDO_LIST);
            } else {
                show_message(ui, "删除失败", error, VIEW_FIDO_LIST);
            }
        }
    } else if(ui->view == VIEW_FIDO_IMPORT_CONFIRM) {
        if(key == MAINTENANCE_KEY_BACK) {
            close_msgbox(ui);
            render_fido_menu(ui);
        } else if(key == MAINTENANCE_KEY_ENTER) {
            char error[256];
            int imported;
            close_msgbox(ui);
            imported = fido_store_import(FIDO_STORE_DIR, FIDO_STORE_TAR_PATH,
                                         error, sizeof(error));
            fido_reload(ui);
            if(imported < 0) {
                show_message(ui, "导入失败", error, VIEW_FIDO_MENU);
            } else {
                char text[128];
                snprintf(text, sizeof(text), "已导入 %d 个密钥。", imported);
                show_message(ui, "导入成功", text, VIEW_FIDO_MENU);
            }
        }
    } else if(ui->view == VIEW_MESSAGE &&
              (key == MAINTENANCE_KEY_ENTER || key == MAINTENANCE_KEY_BACK)) {
        return_from_modal(ui);
    }
}

void maintenance_ui_tick(maintenance_ui_t *ui)
{
    sd_format_phase_t before;
    unsigned int progress;
    if(!ui || ui->view != VIEW_FORMAT_PROGRESS ||
       sd_format_finished(&ui->formatter)) return;
    before = sd_format_phase(&ui->formatter);
    progress = sd_format_progress(&ui->formatter);
    sd_format_poll(&ui->formatter);
    if(before != sd_format_phase(&ui->formatter) ||
       progress != sd_format_progress(&ui->formatter))
        update_format_progress(ui);
}

bool maintenance_ui_should_exit(const maintenance_ui_t *ui)
{
    return ui == NULL || ui->should_exit;
}

void maintenance_ui_destroy(maintenance_ui_t *ui)
{
    if(!ui) return;
    close_msgbox(ui);
    lv_style_reset(&ui->style_item);
    lv_style_reset(&ui->style_item_focused);
    if(ui->font) lv_freetype_font_delete(ui->font);
    free(ui->fido_keys);
    free(ui->config);
    free(ui);
}
