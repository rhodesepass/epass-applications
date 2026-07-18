/*
 * 播放条 + 四键状态机。
 *
 * 状态：HIDDEN(全屏视频) / SHOWN(条可见) / SCRUB(拖时间轴中)。
 * 键位：ENTER 短按=播放/暂停(SCRUB 中=确认并退出拖动)，ENTER 长按=旋转
 * 循环切换，PREV/NEXT=进入/步进 SCRUB(按住连续步进并加速)，BACK=逐层退出
 * (SCRUB→取消回原位 / 条→隐藏 / 全屏→退出应用)。
 *
 * SCRUB 期间强制暂停：播放不再顶着用户跑，停手 VP_SCRUB_COMMIT_MS 后
 * seek 预览目标关键帧(靠 mediaplayer 的暂停单步)，目标位置一直以用户
 * 手里的 scrub_target 为准，与实际播放位置解耦；确认/取消时恢复进入前
 * 的播放暂停状态。播放中 VP_BAR_AUTOHIDE_MS 无操作自动隐藏。
 *
 * 字体：epass_fonts 注册目录的思源黑体(FreeType)——共享 lvgl 只编了
 * Montserrat 14/16，大字号和中文说明都得靠它；加载失败退回默认字体
 * 并把说明降级成 ASCII。
 */
#include "ui.h"
#include "config.h"

#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FONT_REGISTRY_DIR
#define FONT_REGISTRY_DIR "/usr/share/fonts/epass" /* dev fallback */
#endif

typedef enum {
    UI_HIDDEN = 0,
    UI_SHOWN,
    UI_SCRUB,
} ui_state_t;

struct vp_ui {
    vp_platform_t *platform;
    mediaplayer_t *mp;

    ui_state_t state;
    bool should_exit;

    lv_font_t *font_big;
    lv_font_t *font_help;

    lv_obj_t *state_label;
    lv_obj_t *time_label;
    lv_obj_t *rot_label;
    lv_obj_t *slider;
    lv_obj_t *help_label;
    lv_timer_t *refresh_timer;

    uint32_t last_activity_ms;

    /* 长按/自持连按(不依赖内核 auto-repeat) */
    vp_key_t held_key;
    uint32_t held_since_ms;
    uint32_t next_step_ms;
    bool long_fired;

    /* SCRUB 会话：目标位置只归用户，播放位置不来抢 */
    uint32_t scrub_target_ms;
    uint32_t scrub_origin_ms;   /* 进入时的位置，取消时回这 */
    uint32_t scrub_last_key_ms;
    bool scrub_was_paused;
    bool scrub_dirty;           /* 有未提交的步进 */
    bool scrub_any_commit;      /* 预览 seek 已动过码流 */
};

static void ui_mark_activity(vp_ui_t *ui)
{
    ui->last_activity_ms = lv_tick_get();
}

static void ui_format_time(char *buf, size_t len, uint32_t pos_ms, uint32_t dur_ms)
{
    snprintf(buf, len, "%02u:%02u/%02u:%02u",
             pos_ms / 60000, pos_ms / 1000 % 60,
             dur_ms / 60000, dur_ms / 1000 % 60);
}

static void ui_refresh(lv_timer_t *timer)
{
    vp_ui_t *ui = lv_timer_get_user_data(timer);
    uint32_t dur = mediaplayer_get_duration_ms(ui->mp);
    uint32_t pos = ui->state == UI_SCRUB ? ui->scrub_target_ms
                                         : mediaplayer_get_position_ms(ui->mp);
    mp_status_t st = mediaplayer_get_status(ui->mp);
    bool cn = ui->font_big != NULL;
    char buf[32];
    int angle;

    if (st == MP_STATUS_ERROR || st == MP_STATUS_STOPPED) {
        ui->should_exit = true;
        return;
    }

    lv_label_set_text_static(ui->state_label,
                             ui->state == UI_SCRUB  ? (cn ? "拖动" : "SEEK") :
                             st == MP_STATUS_PAUSED ? (cn ? "暂停" : "PAUSE") :
                                                      (cn ? "播放" : "PLAY"));
    ui_format_time(buf, sizeof(buf), pos, dur);
    lv_label_set_text(ui->time_label, buf);

    angle = mediaplayer_get_rotation(ui->mp);
    /* 思源黑体有 °，但字表子集不保证收录，用 R 前缀标角度 */
    snprintf(buf, sizeof(buf), "R%d", angle);
    lv_label_set_text(ui->rot_label, buf);

    lv_slider_set_value(ui->slider,
                        dur ? (int32_t)((uint64_t)pos * 1000 / dur) : 0,
                        LV_ANIM_OFF);
}

static void ui_show_bar(vp_ui_t *ui)
{
    if (ui->state == UI_HIDDEN)
        ui->state = UI_SHOWN;
    vp_platform_show_bar(ui->platform, true);
    ui_mark_activity(ui);
}

static void ui_hide_bar(vp_ui_t *ui)
{
    ui->state = UI_HIDDEN;
    vp_platform_show_bar(ui->platform, false);
}

static void ui_enter_scrub(vp_ui_t *ui)
{
    if (ui->state == UI_SCRUB)
        return;
    ui->scrub_origin_ms = mediaplayer_get_position_ms(ui->mp);
    ui->scrub_target_ms = ui->scrub_origin_ms;
    ui->scrub_was_paused = mediaplayer_get_paused(ui->mp);
    ui->scrub_dirty = false;
    ui->scrub_any_commit = false;
    /* 冻住播放：目标位置只跟按键走，视频不来抢 */
    mediaplayer_set_paused(ui->mp, true);
    ui_show_bar(ui);
    ui->state = UI_SCRUB;
}

static void ui_exit_scrub(vp_ui_t *ui, bool cancel)
{
    if (ui->state != UI_SCRUB)
        return;
    if (cancel) {
        if (ui->scrub_any_commit)
            mediaplayer_seek_ms(ui->mp, ui->scrub_origin_ms);
    } else if (ui->scrub_dirty) {
        mediaplayer_seek_ms(ui->mp, ui->scrub_target_ms);
    }
    mediaplayer_set_paused(ui->mp, ui->scrub_was_paused);
    ui->state = UI_SHOWN;
    ui_mark_activity(ui);
}

static void ui_scrub_step(vp_ui_t *ui, int dir, uint32_t step_ms)
{
    uint32_t dur = mediaplayer_get_duration_ms(ui->mp);

    ui_enter_scrub(ui);
    if (dir < 0)
        ui->scrub_target_ms = ui->scrub_target_ms > step_ms ?
                              ui->scrub_target_ms - step_ms : 0;
    else
        ui->scrub_target_ms += step_ms;
    if (dur && ui->scrub_target_ms >= dur)
        ui->scrub_target_ms = dur - 1;

    ui->scrub_dirty = true;
    ui->scrub_last_key_ms = lv_tick_get();
    ui_mark_activity(ui);
    ui_refresh(ui->refresh_timer);
}

static void ui_toggle_pause(vp_ui_t *ui)
{
    mediaplayer_set_paused(ui->mp, !mediaplayer_get_paused(ui->mp));
    ui_show_bar(ui);
}

static void ui_cycle_rotation(vp_ui_t *ui)
{
    static const int next[4] = { 90, 180, 270, 0 };
    int cur = mediaplayer_get_rotation(ui->mp);
    int i = cur == 90 ? 0 : cur == 180 ? 1 : cur == 270 ? 2 : 3;

    mediaplayer_set_rotation(ui->mp, next[(i + 1) % 4]);
    ui_show_bar(ui);
}

void vp_ui_handle_key_event(vp_ui_t *ui, const vp_key_event_t *ev)
{
    if (ev->repeat)
        return; /* 连按节奏由 vp_ui_tick 自己掌握 */

    if (ev->pressed) {
        ui->held_key = ev->key;
        ui->held_since_ms = lv_tick_get();
        ui->next_step_ms = 0;
        ui->long_fired = false;

        switch (ev->key) {
        case VP_KEY_PREV:
            ui_scrub_step(ui, -1, VP_SCRUB_STEP_MS);
            break;
        case VP_KEY_NEXT:
            ui_scrub_step(ui, 1, VP_SCRUB_STEP_MS);
            break;
        case VP_KEY_BACK:
            if (ui->state == UI_SCRUB)
                ui_exit_scrub(ui, true);
            else if (ui->state == UI_SHOWN)
                ui_hide_bar(ui);
            else
                ui->should_exit = true;
            break;
        default:
            break; /* ENTER 短按/长按在抬起/tick 时区分 */
        }
        return;
    }

    /* 抬起 */
    if (ev->key == ui->held_key)
        ui->held_key = VP_KEY_NONE;
    if (ev->key == VP_KEY_ENTER && !ui->long_fired) {
        if (ui->state == UI_SCRUB)
            ui_exit_scrub(ui, false);
        else
            ui_toggle_pause(ui);
    }
}

void vp_ui_tick(vp_ui_t *ui)
{
    uint32_t now = lv_tick_get();

    /* ENTER 长按 = 旋转切换(每次按压只触发一次) */
    if (ui->held_key == VP_KEY_ENTER && !ui->long_fired &&
        now - ui->held_since_ms >= VP_KEY_LONGPRESS_MS) {
        ui->long_fired = true;
        ui_cycle_rotation(ui);
    }

    /* PREV/NEXT 按住连续步进：400ms 起步 150ms 一格，2s 后换大步 */
    if (ui->held_key == VP_KEY_PREV || ui->held_key == VP_KEY_NEXT) {
        uint32_t held = now - ui->held_since_ms;
        if (held >= 400 && (!ui->next_step_ms || now >= ui->next_step_ms)) {
            ui_scrub_step(ui, ui->held_key == VP_KEY_PREV ? -1 : 1,
                          held >= 2000 ? VP_SCRUB_STEP_MS * 3 : VP_SCRUB_STEP_MS);
            ui->next_step_ms = now + 150;
        }
    }

    if (ui->state == UI_SCRUB && ui->held_key == VP_KEY_NONE) {
        uint32_t idle = now - ui->scrub_last_key_ms;

        /* 停手先预览目标关键帧(暂停中 seek 会单步放一帧) */
        if (ui->scrub_dirty && idle >= VP_SCRUB_COMMIT_MS) {
            mediaplayer_seek_ms(ui->mp, ui->scrub_target_ms);
            ui->scrub_dirty = false;
            ui->scrub_any_commit = true;
        }
        /* 预览后再无操作，视为确认，恢复原播放状态 */
        if (!ui->scrub_dirty && idle >= VP_SCRUB_EXIT_MS)
            ui_exit_scrub(ui, false);
    }

    if (ui->state == UI_SHOWN && !mediaplayer_get_paused(ui->mp) &&
        now - ui->last_activity_ms >= VP_BAR_AUTOHIDE_MS)
        ui_hide_bar(ui);
}

bool vp_ui_should_exit(vp_ui_t *ui)
{
    return ui->should_exit;
}

#ifndef FONT_BODY_PATH
#define FONT_BODY_PATH NULL /* 纯 dev 构建无 epass-fonts .pc */
#endif

/*
 * 正文角色(body)字体。角色 -> 文件的映射归 epass-fonts 的 roles.conf 管，
 * 构建期经 .pc 透传成 FONT_BODY_PATH，应用不写死文件名。优先级与
 * quick_start 一致：EPASS_FONTS_DIR 环境覆盖 > 角色路径 > 注册目录拼名。
 */
static lv_font_t *ui_load_body_font(int px)
{
    char buf[512];
    const char *role_path = FONT_BODY_PATH;
    const char *dir = getenv("EPASS_FONTS_DIR"); /* dev/preview 覆盖 */
    const char *path;

    if (dir) {
        snprintf(buf, sizeof(buf), "%s/SourceHanSansSC-Regular.otf", dir);
        path = buf;
    } else if (role_path && access(role_path, R_OK) == 0) {
        path = role_path;
    } else {
        snprintf(buf, sizeof(buf), "%s/SourceHanSansSC-Regular.otf",
                 FONT_REGISTRY_DIR);
        path = buf;
    }
    return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   (uint32_t)px, LV_FREETYPE_FONT_STYLE_NORMAL);
}

vp_ui_t *vp_ui_create(vp_platform_t *platform, mediaplayer_t *mp)
{
    vp_ui_t *ui = calloc(1, sizeof(*ui));
    lv_obj_t *scr;
    int pad, scale_num;

    if (!ui)
        return NULL;
    ui->platform = platform;
    ui->mp = mp;
    ui->state = UI_HIDDEN;

    scale_num = platform->width; /* 字号随屏宽等比(360 基准) */
    ui->font_big = ui_load_body_font(VP_FONT_PX * scale_num / VP_BAR_WIDTH_REF);
    ui->font_help = ui_load_body_font(VP_FONT_HELP_PX * scale_num / VP_BAR_WIDTH_REF);

    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1c1e24), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    pad = platform->bar_height / 12;

    ui->state_label = lv_label_create(scr);
    lv_label_set_text_static(ui->state_label, ui->font_big ? "播放" : "PLAY");
    lv_obj_set_style_text_color(ui->state_label, lv_color_hex(0xffffff), 0);
    if (ui->font_big)
        lv_obj_set_style_text_font(ui->state_label, ui->font_big, 0);
    lv_obj_align(ui->state_label, LV_ALIGN_TOP_LEFT, pad, pad);

    ui->time_label = lv_label_create(scr);
    lv_label_set_text_static(ui->time_label, "00:00/00:00");
    lv_obj_set_style_text_color(ui->time_label, lv_color_hex(0xd0d4dc), 0);
    if (ui->font_big)
        lv_obj_set_style_text_font(ui->time_label, ui->font_big, 0);
    lv_obj_align(ui->time_label, LV_ALIGN_TOP_MID, pad, pad);

    ui->rot_label = lv_label_create(scr);
    lv_label_set_text_static(ui->rot_label, "R0");
    lv_obj_set_style_text_color(ui->rot_label, lv_color_hex(0x8a93a5), 0);
    if (ui->font_big)
        lv_obj_set_style_text_font(ui->rot_label, ui->font_big, 0);
    lv_obj_align(ui->rot_label, LV_ALIGN_TOP_RIGHT, -pad, pad);

    ui->slider = lv_slider_create(scr);
    lv_slider_set_range(ui->slider, 0, 1000);
    lv_obj_remove_flag(ui->slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ui->slider, platform->bar_width - 2 * pad - 8,
                    platform->bar_height / 11);
    lv_obj_align(ui->slider, LV_ALIGN_CENTER, 0, platform->bar_height / 8);
    lv_obj_set_style_bg_color(ui->slider, lv_color_hex(0x3a4050), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->slider, lv_color_hex(0x5aa0ff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui->slider, lv_color_hex(0xffffff), LV_PART_KNOB);

    ui->help_label = lv_label_create(scr);
    if (ui->font_help) {
        lv_obj_set_style_text_font(ui->help_label, ui->font_help, 0);
        lv_label_set_text_static(ui->help_label,
                                 "键1/2 拖动  键3 暂停/播放  长按3 旋转  键4 返回");
    } else {
        lv_label_set_text_static(ui->help_label,
                                 "1/2 seek  3 play/pause  hold3 rotate  4 back");
    }
    lv_obj_set_style_text_color(ui->help_label, lv_color_hex(0x6b7485), 0);
    lv_obj_align(ui->help_label, LV_ALIGN_BOTTOM_MID, 0, -pad);

    ui->refresh_timer = lv_timer_create(ui_refresh, 200, ui);
    ui_mark_activity(ui);

    /* 开播先亮一次条，让用户看到状态与键位说明 */
    ui_show_bar(ui);
    return ui;
}

void vp_ui_destroy(vp_ui_t *ui)
{
    if (!ui)
        return;
    if (ui->refresh_timer)
        lv_timer_delete(ui->refresh_timer);
    if (ui->font_big)
        lv_freetype_font_delete(ui->font_big);
    if (ui->font_help)
        lv_freetype_font_delete(ui->font_help);
    free(ui);
}
