#define _POSIX_C_SOURCE 200809L

#include "ui.h"
#include "../system/sysinfo.h"
#include "../system/ve_check.h"

#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef FONT_REGISTRY_DIR
#define FONT_REGISTRY_DIR "/usr/share/fonts/epass" /* dev fallback */
#endif

#define COMMUNITY_URL "https://epm.iccmc.cc"

/* 配色: 贴近主程序深色主题 */
#define COL_BG     lv_color_hex(0x15181d)
#define COL_TEXT   lv_color_hex(0xf2f5f8)
#define COL_MUTED  lv_color_hex(0xa8b2bd)
#define COL_ACCENT lv_color_hex(0x78c8ff)
#define COL_OK     lv_color_hex(0x62c96a)
#define COL_WARN   lv_color_hex(0xffa040)

typedef enum {
    PAGE_KEYMAP = 0,
    PAGE_SELFTEST,
    PAGE_PLAYBACK,
    PAGE_MAINMENU,
    PAGE_OPLIST,
    PAGE_APPS,
    PAGE_SETTINGS,
    PAGE_USB,
    PAGE_SYSINFO,
    PAGE_MAINTENANCE,
    PAGE_COMMUNITY,
    PAGE_COUNT
} page_id_t;

typedef enum { ARROW_LEFT, ARROW_RIGHT, ARROW_DOWN_LEFT, ARROW_UP_LEFT } arrow_dir_t;

#define MAX_ARROWS 8

struct tutorial_ui {
    tutorial_platform_t *platform;
    lv_font_t *font_title, *font_body, *font_small;
    lv_font_t *font_h1, *font_key; /* 第 1 页专用大字号 */
    lv_obj_t *root;
    lv_obj_t *page;       /* 内容容器, 翻页时 clean 重建 */
    lv_obj_t *hint, *pageno;
    int page_index;
    bool should_exit;
    bool is_720;
    char base_dir[192]; /* 资源根: 可执行文件所在目录 (启动器不保证 cwd) */

    /* 字体子集不保证有箭头字形, lv_line 自画; 点数组须比 lv_line 活得久 */
    lv_point_precise_t arrow_pts[MAX_ARROWS][5];
    int arrow_count;

    /* 第 2 页自检 (VE 检测跑在工作线程, tick 里轮询回填) */
    ve_check_result_t ve;
    bool ve_started, ve_shown;
    bool warn_static;               /* VE 之外的检查是否已有 WARNING */
    lv_obj_t *ve_value, *ve_detail; /* 仅在自检页存活期间有效 */
    lv_obj_t *summary;
};

static int scaled(const tutorial_ui_t *ui, int value)
{
    return value * ui->platform->width / 360;
}

static lv_font_t *load_font(const char *file, int px)
{
    char path[512];
    const char *dir = getenv("EPASS_FONTS_DIR"); /* dev/preview 覆盖 */
    snprintf(path, sizeof(path), "%s/%s", dir ? dir : FONT_REGISTRY_DIR, file);
    if(access(path, R_OK) != 0)
        snprintf(path, sizeof(path), "/usr/share/fonts/epass/%s", file);
    return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   (uint32_t)px, LV_FREETYPE_FONT_STYLE_NORMAL);
}

static lv_obj_t *make_label(tutorial_ui_t *ui, lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    (void)ui;
    return label;
}

/* 在 (x,y) 处画一个 s×s 的箭头 (折线: 头上沿→尖→头下沿→尖→杆尾) */
static void make_arrow_color(tutorial_ui_t *ui, lv_obj_t *parent, arrow_dir_t dir,
                             int x, int y, int s, lv_color_t color)
{
    if(ui->arrow_count >= MAX_ARROWS) return;
    lv_point_precise_t *p = ui->arrow_pts[ui->arrow_count++];
    switch(dir) {
    case ARROW_LEFT:
        p[0] = (lv_point_precise_t){ s * 35 / 100, s * 10 / 100 };
        p[1] = (lv_point_precise_t){ 0, s / 2 };
        p[2] = (lv_point_precise_t){ s * 35 / 100, s * 90 / 100 };
        p[3] = (lv_point_precise_t){ 0, s / 2 };
        p[4] = (lv_point_precise_t){ s, s / 2 };
        break;
    case ARROW_RIGHT:
        p[0] = (lv_point_precise_t){ s * 65 / 100, s * 10 / 100 };
        p[1] = (lv_point_precise_t){ s, s / 2 };
        p[2] = (lv_point_precise_t){ s * 65 / 100, s * 90 / 100 };
        p[3] = (lv_point_precise_t){ s, s / 2 };
        p[4] = (lv_point_precise_t){ 0, s / 2 };
        break;
    case ARROW_DOWN_LEFT:
        p[0] = (lv_point_precise_t){ 0, s / 2 };
        p[1] = (lv_point_precise_t){ 0, s };
        p[2] = (lv_point_precise_t){ s / 2, s };
        p[3] = (lv_point_precise_t){ 0, s };
        p[4] = (lv_point_precise_t){ s, 0 };
        break;
    case ARROW_UP_LEFT:
        p[0] = (lv_point_precise_t){ 0, s / 2 };
        p[1] = (lv_point_precise_t){ 0, 0 };
        p[2] = (lv_point_precise_t){ s / 2, 0 };
        p[3] = (lv_point_precise_t){ 0, 0 };
        p[4] = (lv_point_precise_t){ s, s };
        break;
    }
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, p, 5);
    lv_obj_set_style_line_width(line, scaled(ui, 3), 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_pos(line, x, y);
}

static void make_arrow(tutorial_ui_t *ui, lv_obj_t *parent, arrow_dir_t dir,
                       int x, int y, int s)
{
    make_arrow_color(ui, parent, dir, x, y, s, COL_ACCENT);
}

/* ---------------- 第 1 页: 按键位置 ---------------- */

static void build_page_keymap(tutorial_ui_t *ui)
{
    const int w = ui->platform->width, h = ui->platform->height;
    const int arrow = scaled(ui, 20);

    /* 欢迎语避开按键列: 720p 列占右侧上半屏 → 居中下移; 360p 列占左侧 → 放右上空区 */
    lv_obj_t *title = make_label(ui, ui->page, "欢迎使用\n电子通行证", ui->font_h1, COL_ACCENT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    if(ui->is_720) lv_obj_align(title, LV_ALIGN_CENTER, 0, scaled(ui, 96));
    else lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -scaled(ui, 16), scaled(ui, 20));

    lv_obj_t *go = make_label(ui, ui->page, "请按 KEY_3 继续\n按 KEY_4 跳过", ui->font_key, COL_TEXT);
    lv_obj_set_style_text_align(go, LV_TEXT_ALIGN_CENTER, 0);
    if(ui->is_720) lv_obj_align(go, LV_ALIGN_CENTER, 0, scaled(ui, 160));
    else lv_obj_align(go, LV_ALIGN_TOP_RIGHT, -scaled(ui, 28), scaled(ui, 120));

    if(!ui->is_720) {
        /* 360p 机种, 按真机照片实测对齐 (屏幕坐标, 中心 y):
         * KEY_1 在屏幕上沿之上 → 顶角 ↖; KEY_2~KEY_4/刷机 约 34/127/220/313;
         * SD 卡槽 (非按键, 弱化显示) 约 451; 电源在左下角。 */
        make_arrow(ui, ui->page, ARROW_UP_LEFT, scaled(ui, 4), scaled(ui, 2), arrow);
        lv_obj_t *l = make_label(ui, ui->page, "KEY_1  上翻", ui->font_key, COL_TEXT);
        lv_obj_set_pos(l, scaled(ui, 32), scaled(ui, 2));

        static const struct { const char *text; int center; } rows[] = {
            { "KEY_2  下翻", 34 }, { "KEY_3  确定", 127 },
            { "KEY_4  退出", 220 }, { "刷机", 313 },
        };
        for(size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            int c = scaled(ui, rows[i].center);
            make_arrow(ui, ui->page, ARROW_LEFT, scaled(ui, 4), c - arrow / 2, arrow);
            l = make_label(ui, ui->page, rows[i].text, ui->font_key, COL_TEXT);
            /* KEY_2 物理位置贴顶, 标签下让一点避免和 KEY_1 行叠 */
            int ly = c - scaled(ui, 12);
            int min_ly = scaled(ui, 28);
            lv_obj_set_pos(l, scaled(ui, 32), ly < min_ly ? min_ly : ly);
        }

        make_arrow_color(ui, ui->page, ARROW_LEFT, scaled(ui, 4),
                         scaled(ui, 451) - arrow / 2, arrow, COL_MUTED);
        l = make_label(ui, ui->page, "SD 卡槽", ui->font_key, COL_MUTED);
        lv_obj_set_pos(l, scaled(ui, 32), scaled(ui, 451) - scaled(ui, 12));

        make_arrow(ui, ui->page, ARROW_DOWN_LEFT, scaled(ui, 4), h - scaled(ui, 56), arrow);
        lv_obj_t *pw = make_label(ui, ui->page, "电源", ui->font_key, COL_TEXT);
        lv_obj_set_pos(pw, scaled(ui, 32), h - scaled(ui, 58));
    } else {
        /* 720p 机种: 按键在机身右侧, 从上到 50% 屏高等距; KEY_4 与刷机共用 */
        static const char *rows[] = { "电源", "上翻  KEY_1", "下翻  KEY_2", "确定  KEY_3", "退出/刷机  KEY_4" };
        int span = h * 50 / 100 - arrow;
        for(int i = 0; i < 5; i++) {
            int y = span * i / 4;
            make_arrow(ui, ui->page, ARROW_RIGHT, w - arrow - scaled(ui, 4), y, arrow);
            lv_obj_t *l = make_label(ui, ui->page, rows[i], ui->font_key, COL_TEXT);
            lv_obj_update_layout(l);
            lv_obj_set_pos(l, w - scaled(ui, 32) - lv_obj_get_width(l), y - scaled(ui, 2));
        }
    }

    lv_obj_t *note = make_label(ui, ui->page,
        "KEY_1 上翻/上一项    KEY_2 下翻/下一项\n"
        "KEY_3 确定/进入    KEY_4 取消/退出\n"
        "刷机: 系统断电时拔出 SD 卡, 按住刷机键和\n"
        "电源键开机, 即可进入刷机模式。",
        ui->font_small, COL_MUTED);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -scaled(ui, 6));
}

/* ---------------- 第 2 页: 设备自检 ---------------- */

static void update_summary(tutorial_ui_t *ui)
{
    if(!ui->summary) return;
    ve_check_status_t s = atomic_load(&ui->ve.status);
    if(s == VE_CHECK_PENDING) {
        lv_label_set_text(ui->summary, "环境检测中…");
        lv_obj_set_style_text_color(ui->summary, COL_MUTED, 0);
    } else if(ui->warn_static || s != VE_CHECK_OK) {
        lv_label_set_text(ui->summary, "环境检查和预期不一致，程序可能无法正常运行。");
        lv_obj_set_style_text_color(ui->summary, COL_WARN, 0);
    } else {
        lv_label_set_text(ui->summary, "环境检测通过");
        lv_obj_set_style_text_color(ui->summary, COL_OK, 0);
    }
}

static void ve_show_result(tutorial_ui_t *ui)
{
    if(!ui->ve_value) return;
    switch(atomic_load(&ui->ve.status)) {
    case VE_CHECK_OK:
        lv_label_set_text(ui->ve_value, "OK");
        lv_obj_set_style_text_color(ui->ve_value, COL_OK, 0);
        break;
    case VE_CHECK_DEFECTIVE:
        lv_label_set_text(ui->ve_value, "WARNING");
        lv_obj_set_style_text_color(ui->ve_value, COL_WARN, 0);
        break;
    case VE_CHECK_UNKNOWN: /* 未知也按异常算 */
        lv_label_set_text(ui->ve_value, "WARNING (UNKNOWN)");
        lv_obj_set_style_text_color(ui->ve_value, COL_WARN, 0);
        break;
    default:
        return; /* 还在检测 */
    }
    lv_label_set_text(ui->ve_detail, ui->ve.detail);
    update_summary(ui);
    ui->ve_shown = true;
}

/* 一行 "标题 + 值": 返回下一行 y */
static int selftest_row(tutorial_ui_t *ui, int x, int y, const char *caption,
                        const char *value, lv_color_t color)
{
    lv_obj_t *l = make_label(ui, ui->page, caption, ui->font_small, COL_MUTED);
    lv_obj_set_pos(l, x, y);
    l = make_label(ui, ui->page, value, ui->font_body, color);
    lv_obj_set_pos(l, x + scaled(ui, 110), y - scaled(ui, 3));
    return y + scaled(ui, 30);
}

static void build_page_selftest(tutorial_ui_t *ui)
{
    const int w = ui->platform->width, h = ui->platform->height;
    const int x = scaled(ui, 18);
    int y = scaled(ui, 16);
    ui->warn_static = false;

    lv_obj_t *title = make_label(ui, ui->page, "欢迎使用电子通行证！", ui->font_title, COL_ACCENT);
    lv_obj_set_pos(title, x, y);
    y += scaled(ui, 52);

    /* 设备版本 (device-tree model, 可能较长, 独立成块) */
    char model[96];
    bool model_ok = sysinfo_read_model(model, sizeof(model));
    if(!model_ok) { strcpy(model, "读取失败   WARNING"); ui->warn_static = true; }
    lv_obj_t *l = make_label(ui, ui->page, "设备版本", ui->font_small, COL_MUTED);
    lv_obj_set_pos(l, x, y);
    y += scaled(ui, 20);
    l = make_label(ui, ui->page, model, ui->font_body, model_ok ? COL_TEXT : COL_WARN);
    lv_obj_set_width(l, w - 2 * x);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, x, y);
    y += scaled(ui, 36);

    /* 机种类型: 只认 360p / 720p 两档 */
    char text[64];
    bool screen_ok = (w == 360 && h == 640) || (w == 720 && h == 1280);
    if(screen_ok)
        snprintf(text, sizeof(text), "%dp (%dx%d)", w, w, h);
    else
        snprintf(text, sizeof(text), "%dx%d   WARNING", w, h);
    if(!screen_ok) ui->warn_static = true;
    y = selftest_row(ui, x, y, "机种类型", text, screen_ok ? COL_TEXT : COL_WARN);

    /* 启动方式 */
    sysinfo_boot_t boot = sysinfo_read_boot_source();
    bool boot_ok = boot != SYSINFO_BOOT_UNKNOWN;
    if(!boot_ok) ui->warn_static = true;
    y = selftest_row(ui, x, y, "启动方式",
                     boot == SYSINFO_BOOT_NAND ? "NAND" :
                     boot == SYSINFO_BOOT_SD ? "SD 卡" : "未知   WARNING",
                     boot_ok ? COL_TEXT : COL_WARN);

    /* 实际内存 */
    unsigned mb = 0;
    bool mem_ok = false;
    if(sysinfo_read_memory_mb(&mb)) {
        mem_ok = sysinfo_mem_is_expected(mb);
        snprintf(text, sizeof(text), "%u MB   %s", mb, mem_ok ? "OK" : "WARNING");
    } else {
        snprintf(text, sizeof(text), "读取失败   WARNING");
    }
    if(!mem_ok) ui->warn_static = true;
    y = selftest_row(ui, x, y, "实际内存", text, mem_ok ? COL_OK : COL_WARN);

    /* 视频解码器 (异步) */
    l = make_label(ui, ui->page, "视频解码器", ui->font_small, COL_MUTED);
    lv_obj_set_pos(l, x, y);
    ui->ve_value = make_label(ui, ui->page, "检测中…", ui->font_body, COL_TEXT);
    lv_obj_set_pos(ui->ve_value, x + scaled(ui, 110), y - scaled(ui, 3));
    y += scaled(ui, 28);
    ui->ve_detail = make_label(ui, ui->page, "", ui->font_small, COL_MUTED);
    lv_obj_set_width(ui->ve_detail, w - 2 * x);
    lv_label_set_long_mode(ui->ve_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(ui->ve_detail, x, y);
    y += scaled(ui, 40);

    /* 汇总 */
    ui->summary = make_label(ui, ui->page, "", ui->font_body, COL_MUTED);
    lv_obj_set_width(ui->summary, w - 2 * x);
    lv_label_set_long_mode(ui->summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(ui->summary, x, y);

    if(!ui->ve_started) {
        ui->ve_started = true;
        ve_check_start(&ui->ve); /* 教程独占前台, 此时视频解码必然停着, 可安全扰动 PLL_VE */
    }
    ui->ve_shown = false;
    ve_show_result(ui); /* 回看本页时可能已出结果 */
    update_summary(ui);

    lv_obj_t *foot = make_label(ui, ui->page,
        "本项目是开源的自由硬件与软件,\n"
        "源代码可以从 github.com/rhodesepass 获取。",
        ui->font_small, COL_MUTED);
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -scaled(ui, 6));
}

/* ---------------- 第 3~8 页: 主程序各屏 (截图 + 要点) ---------------- */

static void build_shot_page(tutorial_ui_t *ui, const char *title_text,
                            const char *shot_name, const char *bullets)
{
    const int w = ui->platform->width;
    const int x = scaled(ui, 18);

    lv_obj_t *title = make_label(ui, ui->page, title_text, ui->font_title, COL_ACCENT);
    lv_obj_set_pos(title, x, scaled(ui, 10));

    int img_w = scaled(ui, 180);
    int img_h = img_w * ui->platform->height / w;
    char src[288];
    snprintf(src, sizeof(src), "A:%s/assets/shots/%d/%s.jpg", ui->base_dir, img_w, shot_name);
    lv_obj_t *img = lv_image_create(ui->page);
    lv_image_set_src(img, src);
    lv_obj_set_style_border_width(img, scaled(ui, 2), 0);
    lv_obj_set_style_border_color(img, lv_color_hex(0x3a4350), 0);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, scaled(ui, 52));

    lv_obj_t *text = make_label(ui, ui->page, bullets, ui->font_body, COL_TEXT);
    lv_label_set_recolor(text, true); /* 支持 #RRGGBB xx# 高亮 (设备无粗体字重) */
    lv_obj_set_width(text, w - 2 * x);
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(text, scaled(ui, 4), 0);
    lv_obj_set_pos(text, x, scaled(ui, 52) + img_h + scaled(ui, 14));
}

/* ---------------- 最后一页: 社群二维码 ---------------- */

static void build_page_community(tutorial_ui_t *ui)
{
    lv_obj_t *title = make_label(ui, ui->page,
        "在线管理设备、下载APP、功能建议、\n加入社群、更多玩法…",
        ui->font_body, COL_TEXT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, scaled(ui, 24));

    lv_obj_t *scan = make_label(ui, ui->page, "请扫描下方二维码", ui->font_title, COL_ACCENT);
    lv_obj_align(scan, LV_ALIGN_TOP_MID, 0, scaled(ui, 78));

    /* 白底留边, 保证二维码静区可扫 */
    int qr_size = scaled(ui, 190);
    int pad = scaled(ui, 12);
    lv_obj_t *panel = lv_obj_create(ui->page);
    lv_obj_set_size(panel, qr_size + 2 * pad, qr_size + 2 * pad);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, scaled(ui, 6), 0);
    lv_obj_set_style_pad_all(panel, pad, 0);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, scaled(ui, 10));

    lv_obj_t *qr = lv_qrcode_create(panel);
    lv_qrcode_set_size(qr, qr_size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, COMMUNITY_URL, (uint32_t)strlen(COMMUNITY_URL));
    lv_obj_center(qr);

    lv_obj_t *url = make_label(ui, ui->page, COMMUNITY_URL, ui->font_body, COL_TEXT);
    lv_obj_align(url, LV_ALIGN_CENTER, 0, scaled(ui, 135));

    lv_obj_t *done = make_label(ui, ui->page,
        "本教程后续可通过“应用”打开。\n按 KEY_3 完成教程",
        ui->font_small, COL_MUTED);
    lv_obj_set_style_text_align(done, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -scaled(ui, 8));
}

/* ---------------- 翻页 ---------------- */

static void build_page(tutorial_ui_t *ui)
{
    ui->ve_value = ui->ve_detail = ui->summary = NULL; /* 旧页对象即将销毁 */
    ui->arrow_count = 0;
    lv_obj_clean(ui->page);

    switch(ui->page_index) {
    case PAGE_KEYMAP:   build_page_keymap(ui); break;
    case PAGE_SELFTEST: build_page_selftest(ui); break;
    case PAGE_PLAYBACK:
        build_shot_page(ui, "默认画面 (播放态)", "playback",
            "开机后停留的待机画面, 循环播放当前干员的立绘视频。\n"
            "· KEY_1 / KEY_2  打开干员列表\n"
            "· KEY_3  查看扩列图\n"
            "· KEY_4  进入主菜单\n"
            "· 电源键  弹出关机确认");
        break;
    case PAGE_MAINMENU:
        build_shot_page(ui, "主菜单", "mainmenu",
            "播放画面按 KEY_4 进入, 是各功能的总入口。\n"
            "· 六宫格: 干员 / 扩列图 / 应用 / 文件 / 设置 / 设备\n"
            "· 底部: 亮度调节、重启程序、关机\n"
            "· KEY_4  返回播放画面");
        break;
    case PAGE_OPLIST:
        build_shot_page(ui, "干员列表 / 扩列图", "oplist",
            "播放画面按 KEY_1 / KEY_2 直接进入。\n"
            "· 选中干员即切换当前播放的立绘\n"
            "· 扩列图: 播放画面按 KEY_3 进入,\n"
            "  KEY_1 / KEY_2 翻页\n"
            "· 干员素材与扩列图推荐用 Web 管理器\n"
            "  或手机 APP 管理");
        break;
    case PAGE_APPS:
        build_shot_page(ui, "应用 / 文件", "applist",
            "第三方应用与文件管理, 从主菜单进入。\n"
            "· 应用装在 NAND 的 /app 或 SD 卡的 /sd/app\n"
            "· 选中即启动, 应用内一般按 KEY_4 退出\n"
            "· 文件: 浏览设备文件, 可用关联应用打开");
        break;
    case PAGE_SETTINGS:
        build_shot_page(ui, "设置", "settings",
            "调整播放与系统行为。\n"
            "· 切换模式: 顺序 / 随机 / 手动\n"
            "· 自动切换间隔、低电量自动关机、主题等\n"
            "· 下拉框: KEY_3 进入编辑, KEY_1 / KEY_2 选择,\n"
            "  再按 KEY_3 确认");
        break;
    case PAGE_USB:
        build_shot_page(ui, "USB 模式", "usbselect",
            "插入 USB 线时会弹出用途选择。\n"
            "· 管理APP: 连接 Web 管理器或安卓 APP\n"
            "· 文件MTP: 作为 MTP 设备传输文件\n"
            "· FIDO密钥: 模拟 FIDO 密钥, 做着玩的,\n"
            "  请谨慎使用\n"
            "· 要选其他模式: 到设置按“重置USB模式”");
        break;
    case PAGE_SYSINFO:
        build_shot_page(ui, "设备信息", "sysinfo",
            "查看存储占用与版本, 从主菜单“设备”进入。\n"
            "· NAND / SD 卡容量、系统与程序版本\n"
            "· 可在此格式化 SD 卡 (有二次确认)\n"
            "· 二次确认框: KEY_3 确定, KEY_4 取消");
        break;
    case PAGE_MAINTENANCE:
        build_shot_page(ui, "系统维护", "applist",
            "更详细的设置可通过 应用 里的 系统维护 打开:\n"
            "· 格式化存储卡\n"
            "· 设置拓展口功能\n"
            "· 维护 FIDO 令牌\n"
            "· #78c8ff 启动USB高速模式#\n"
            "等进阶功能, 请按屏幕提示谨慎操作。");
        break;
    case PAGE_COMMUNITY: build_page_community(ui); break;
    default: break;
    }

    char pageno[16];
    snprintf(pageno, sizeof(pageno), "%d / %d", ui->page_index + 1, PAGE_COUNT);
    lv_label_set_text(ui->pageno, pageno);
    if(ui->page_index == 0)
        lv_label_set_text(ui->hint, "KEY_3 继续 · KEY_4 跳过");
    else if(ui->page_index == PAGE_COUNT - 1)
        lv_label_set_text(ui->hint, "KEY_1 上一页 · KEY_3 完成");
    else
        lv_label_set_text(ui->hint, "KEY_1 上一页 · KEY_3 下一页 · KEY_4 退出");
}

/* ---------------- 对外接口 ---------------- */

tutorial_ui_t *tutorial_ui_create(tutorial_platform_t *platform)
{
    tutorial_ui_t *ui = calloc(1, sizeof(*ui));
    if(!ui) return NULL;
    ui->platform = platform;
    ui->is_720 = platform->width >= 720;
    /* 资源相对可执行文件目录取; 拿不到或旁边没有 assets (如 host 预览工具
     * 从 build 目录跑) 时回退 cwd */
    ssize_t n = readlink("/proc/self/exe", ui->base_dir, sizeof(ui->base_dir) - 1);
    if(n > 0) {
        ui->base_dir[n] = '\0';
        char *slash = strrchr(ui->base_dir, '/');
        if(slash) *slash = '\0';
    }
    char probe[224];
    snprintf(probe, sizeof(probe), "%s/assets", ui->base_dir);
    if(n <= 0 || access(probe, F_OK) != 0) {
        if(!getcwd(ui->base_dir, sizeof(ui->base_dir))) strcpy(ui->base_dir, ".");
    }

    ui->font_title = load_font("SourceHanSerifSC-Heavy.otf", scaled(ui, 24));
    ui->font_body = load_font("SourceHanSansSC-Regular.otf", scaled(ui, 15));
    ui->font_small = load_font("SourceHanSansSC-Regular.otf", scaled(ui, 12));
    ui->font_h1 = load_font("SourceHanSerifSC-Heavy.otf", scaled(ui, 30));
    ui->font_key = load_font("SourceHanSansSC-Regular.otf", scaled(ui, 19));
    if(!ui->font_title || !ui->font_body || !ui->font_small ||
       !ui->font_h1 || !ui->font_key) {
        tutorial_ui_destroy(ui);
        return NULL;
    }

    ui->root = lv_screen_active();
    lv_obj_set_style_bg_color(ui->root, COL_BG, 0);
    lv_obj_set_style_text_font(ui->root, ui->font_body, 0);

    ui->page = lv_obj_create(ui->root);
    lv_obj_remove_style_all(ui->page);
    lv_obj_set_size(ui->page, platform->width, platform->height - scaled(ui, 24));
    lv_obj_set_pos(ui->page, 0, 0);

    /* 底栏: 左键位提示, 右页码 */
    ui->hint = lv_label_create(ui->root);
    lv_obj_set_style_text_font(ui->hint, ui->font_small, 0);
    lv_obj_set_style_text_color(ui->hint, COL_MUTED, 0);
    lv_obj_align(ui->hint, LV_ALIGN_BOTTOM_LEFT, scaled(ui, 8), -scaled(ui, 4));

    ui->pageno = lv_label_create(ui->root);
    lv_obj_set_style_text_font(ui->pageno, ui->font_small, 0);
    lv_obj_set_style_text_color(ui->pageno, COL_MUTED, 0);
    lv_obj_align(ui->pageno, LV_ALIGN_BOTTOM_RIGHT, -scaled(ui, 8), -scaled(ui, 4));

    build_page(ui);
    return ui;
}

void tutorial_ui_handle_key(tutorial_ui_t *ui, tutorial_key_t key)
{
    switch(key) {
    case TUTORIAL_KEY_PREV:
        if(ui->page_index > 0) { ui->page_index--; build_page(ui); }
        break;
    case TUTORIAL_KEY_NEXT:
    case TUTORIAL_KEY_ENTER:
        if(ui->page_index < PAGE_COUNT - 1) { ui->page_index++; build_page(ui); }
        else if(key == TUTORIAL_KEY_ENTER) ui->should_exit = true;
        break;
    case TUTORIAL_KEY_BACK:
        ui->should_exit = true;
        break;
    default:
        break;
    }
}

void tutorial_ui_tick(tutorial_ui_t *ui)
{
    if(ui->ve_started && !ui->ve_shown && ui->ve_value)
        ve_show_result(ui);
}

bool tutorial_ui_should_exit(const tutorial_ui_t *ui)
{
    return ui->should_exit;
}

void tutorial_ui_destroy(tutorial_ui_t *ui)
{
    if(!ui) return;
    /* VE 检测线程可能还在跑且会写 ui->ve, 等它收尾 (最多 ~1.5s) */
    for(int i = 0; i < 150 && ui->ve_started &&
        atomic_load(&ui->ve.status) == VE_CHECK_PENDING; i++)
        nanosleep(&(struct timespec){ 0, 10000000 }, NULL);
    if(ui->font_title) lv_freetype_font_delete(ui->font_title);
    if(ui->font_body) lv_freetype_font_delete(ui->font_body);
    if(ui->font_small) lv_freetype_font_delete(ui->font_small);
    if(ui->font_h1) lv_freetype_font_delete(ui->font_h1);
    if(ui->font_key) lv_freetype_font_delete(ui->font_key);
    free(ui);
}
