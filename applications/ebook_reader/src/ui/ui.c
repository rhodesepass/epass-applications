#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FONT_REGISTRY_DIR
#define FONT_REGISTRY_DIR "/usr/share/fonts/epass"
#endif

#define UI_MAX_LIST_ITEMS EBOOK_MAX_BOOKMARKS

typedef enum {
    VIEW_READER,
    VIEW_MENU,
    VIEW_JUMP,
    VIEW_BOOKMARK_GOTO,
    VIEW_BOOKMARK_DELETE,
    VIEW_THEME,
    VIEW_BRIGHTNESS,
    VIEW_FONT,
    VIEW_EXIT_CONFIRM
} view_t;

struct ebook_ui {
    ebook_platform_t *platform;
    ebook_document_t *document;
    ebook_state_t *state;

    lv_obj_t *body;
    lv_obj_t *status;
    lv_obj_t *page_label;

    lv_obj_t *list;
    lv_obj_t *list_buttons[UI_MAX_LIST_ITEMS];
    int list_count;

    lv_obj_t *msgbox;
    lv_obj_t *value_label;
    lv_obj_t *slider;
    lv_obj_t *roller;

    lv_style_t style_item;
    lv_style_t style_item_focused;

    lv_font_t *font;    /* 正文字体，跟随“字号”设置 */
    lv_font_t *ui_font; /* 界面字体，固定大小，不随正文字号变化 */
    int line_space;
    view_t view;
    int menu_index;
    int selection;
    size_t jump_page;
    bool should_exit;
};

static const char *menu_items[] = {
    "跳页", "添加书签", "浏览/跳转书签", "删除书签",
    "主题", "亮度", "字号", "返回阅读"
};
#define MENU_ITEM_COUNT ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static const int font_sizes[] = {18, 20, 22, 26};
#define FONT_SIZE_COUNT ((int)(sizeof(font_sizes) / sizeof(font_sizes[0])))

static int scaled(const ebook_ui_t *ui, int base)
{
    int value = base * ui->platform->width / 360;
    return value > 0 ? value : 1;
}

static const char *theme_name(ebook_theme_t theme)
{
    static const char *names[] = {"白昼", "夜间", "护眼"};
    return names[theme];
}

static void theme_colors(const ebook_ui_t *ui, lv_color_t *bg, lv_color_t *fg,
                         lv_color_t *bar)
{
    if(ui->state->theme == EBOOK_THEME_NIGHT) {
        *bg = lv_color_hex(0x101010);
        *fg = lv_color_hex(0xD0D0D0);
        *bar = lv_color_hex(0x202020);
    } else if(ui->state->theme == EBOOK_THEME_EYE) {
        *bg = lv_color_hex(0xC7D9B0);
        *fg = lv_color_hex(0x23351F);
        *bar = lv_color_hex(0xAFC49A);
    } else {
        *bg = lv_color_hex(0xF8F4E8);
        *fg = lv_color_hex(0x202020);
        *bar = lv_color_hex(0xE5DFCE);
    }
}

static void apply_theme(ebook_ui_t *ui)
{
    lv_color_t bg, fg, bar;
    theme_colors(ui, &bg, &fg, &bar);
    lv_obj_set_style_bg_color(lv_screen_active(), bg, 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ui->body, fg, 0);
    lv_obj_set_style_text_color(ui->status, fg, 0);
    lv_obj_set_style_bg_color(ui->status, bar, 0);
    lv_obj_set_style_bg_opa(ui->status, LV_OPA_COVER, 0);
    /* 列表项样式跟随主题，选中项固定用高亮蓝 */
    lv_style_set_bg_color(&ui->style_item, bar);
    lv_style_set_text_color(&ui->style_item, fg);
    lv_obj_report_style_change(&ui->style_item);
}

static void close_msgbox(ebook_ui_t *ui)
{
    if(ui->msgbox != NULL) {
        lv_msgbox_close(ui->msgbox);
        ui->msgbox = NULL;
    }
}

static void clear_body(ebook_ui_t *ui)
{
    close_msgbox(ui);
    lv_obj_clean(ui->body);
    ui->page_label = NULL;
    ui->list = NULL;
    ui->list_count = 0;
    ui->value_label = NULL;
    ui->slider = NULL;
    ui->roller = NULL;
    memset(ui->list_buttons, 0, sizeof(ui->list_buttons));
}

static lv_obj_t *make_list(ebook_ui_t *ui)
{
    lv_obj_t *list = lv_list_create(ui->body);
    lv_obj_set_size(list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    ui->list = list;
    return list;
}

static lv_obj_t *add_list_item(ebook_ui_t *ui, const char *text)
{
    lv_obj_t *button;
    if(ui->list_count >= UI_MAX_LIST_ITEMS) return NULL;
    button = lv_list_add_button(ui->list, NULL, text);
    lv_obj_add_style(button, &ui->style_item, 0);
    lv_obj_add_style(button, &ui->style_item_focused, LV_STATE_FOCUS_KEY);
    ui->list_buttons[ui->list_count++] = button;
    return button;
}

static void select_list_item(ebook_ui_t *ui, int index)
{
    int i;
    for(i = 0; i < ui->list_count; ++i) {
        if(i == index) {
            lv_obj_add_state(ui->list_buttons[i], LV_STATE_FOCUS_KEY);
            lv_obj_scroll_to_view(ui->list_buttons[i], LV_ANIM_OFF);
        } else {
            lv_obj_remove_state(ui->list_buttons[i], LV_STATE_FOCUS_KEY);
        }
    }
}

/* 设置类视图的公共骨架：标题 + 居中控件区 */
static lv_obj_t *make_setting_panel(ebook_ui_t *ui, const char *title)
{
    lv_obj_t *heading;
    clear_body(ui);
    lv_obj_set_flex_flow(ui->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui->body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui->body, scaled(ui, 22), 0);
    heading = lv_label_create(ui->body);
    lv_label_set_text(heading, title);
    return heading;
}

static lv_font_t *load_font(int font_px)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/SourceHanSansSC-Regular.otf", FONT_REGISTRY_DIR);
    if(access(path, R_OK) != 0)
        snprintf(path, sizeof(path), "/usr/share/fonts/epass/SourceHanSansSC-Regular.otf");
    return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   (uint32_t)font_px, LV_FREETYPE_FONT_STYLE_NORMAL);
}

/* 只重建正文字体并重新分页；界面字体固定不动 */
static bool rebuild_font(ebook_ui_t *ui, size_t old_offset)
{
    int font_px = scaled(ui, ui->state->font_px);
    int margin = scaled(ui, 12);
    int status_h = scaled(ui, 34);
    int body_w = ui->platform->width - margin * 2;
    int body_h = ui->platform->height - status_h - margin * 2;
    int line_h = font_px + scaled(ui, 7);
    lv_font_t *font = load_font(font_px);
    if(!font) return false;
    if(ui->font) lv_freetype_font_delete(ui->font);
    ui->font = font;
    ui->line_space = line_h > font->line_height ? line_h - font->line_height : 0;
    if(!ebook_paginate(ui->document, font, body_w, body_h, line_h)) return false;
    ui->document->current_page = ebook_page_for_offset(ui->document, old_offset);
    return true;
}

static void render_reader(ebook_ui_t *ui)
{
    ebook_document_t *doc = ui->document;
    char *text = ebook_page_text(doc, doc->current_page);
    char status[512];
    unsigned percent = doc->length
        ? (unsigned)(doc->pages[doc->current_page] * 100 / doc->length) : 100;
    clear_body(ui);
    ui->page_label = lv_label_create(ui->body);
    lv_obj_set_size(ui->page_label, lv_pct(100), lv_pct(100));
    lv_label_set_long_mode(ui->page_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(ui->page_label, ui->font, 0);
    lv_obj_set_style_text_line_space(ui->page_label, ui->line_space, 0);
    lv_label_set_text(ui->page_label, text ? text : "");
    free(text);
    snprintf(status, sizeof(status), "%s  %zu/%zu  %u%%",
             doc->title, doc->current_page + 1, doc->page_count, percent);
    lv_label_set_text(ui->status, status);
    ui->state->position = doc->pages[doc->current_page];
    ui->view = VIEW_READER;
}

static void render_menu(ebook_ui_t *ui)
{
    int i;
    clear_body(ui);
    make_list(ui);
    for(i = 0; i < MENU_ITEM_COUNT; ++i) {
        char text[128];
        if(i == 4)
            snprintf(text, sizeof(text), "%s：%s", menu_items[i],
                     theme_name(ui->state->theme));
        else if(i == 5)
            snprintf(text, sizeof(text), "%s：%d", menu_items[i],
                     ui->state->brightness);
        else if(i == 6)
            snprintf(text, sizeof(text), "%s：%d", menu_items[i],
                     ui->state->font_px);
        else
            snprintf(text, sizeof(text), "%s", menu_items[i]);
        add_list_item(ui, text);
    }
    select_list_item(ui, ui->menu_index);
    lv_label_set_text(ui->status, "KEY1/2 选择  KEY3 确认  KEY4 返回");
    ui->view = VIEW_MENU;
}

static void update_jump(ebook_ui_t *ui)
{
    unsigned percent = ui->document->page_count > 1
        ? (unsigned)(ui->jump_page * 100 / (ui->document->page_count - 1)) : 100;
    lv_slider_set_value(ui->slider, (int32_t)ui->jump_page, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui->value_label, "%zu / %zu（%u%%）",
                          ui->jump_page + 1, ui->document->page_count, percent);
}

static void render_jump(ebook_ui_t *ui)
{
    make_setting_panel(ui, "跳页");
    ui->value_label = lv_label_create(ui->body);
    ui->slider = lv_slider_create(ui->body);
    lv_obj_set_size(ui->slider, lv_pct(85), scaled(ui, 14));
    lv_slider_set_range(ui->slider, 0,
                        (int32_t)(ui->document->page_count > 0
                                      ? ui->document->page_count - 1 : 0));
    update_jump(ui);
    lv_label_set_text(ui->status, "KEY1/2 调整  KEY3 跳转  KEY4 返回菜单");
    ui->view = VIEW_JUMP;
}

static void render_bookmarks(ebook_ui_t *ui, bool deleting)
{
    clear_body(ui);
    lv_label_set_text(ui->status,
                      deleting ? "KEY3 删除  KEY4 返回" : "KEY3 跳转  KEY4 返回");
    ui->view = deleting ? VIEW_BOOKMARK_DELETE : VIEW_BOOKMARK_GOTO;
    if(ui->state->bookmark_count == 0) {
        lv_obj_t *label = lv_label_create(ui->body);
        lv_label_set_text_fmt(label, "%s\n\n尚无书签",
                              deleting ? "删除书签" : "浏览/跳转书签");
        return;
    }
    make_list(ui);
    for(int i = 0; i < ui->state->bookmark_count; i++) {
        char text[160];
        size_t page = ebook_page_for_offset(ui->document,
                                            ui->state->bookmarks[i].offset);
        snprintf(text, sizeof(text), "%d. 第%zu页  %.64s", i + 1, page + 1,
                 ui->state->bookmarks[i].summary);
        add_list_item(ui, text);
    }
    select_list_item(ui, ui->selection);
}

static void render_theme(ebook_ui_t *ui)
{
    make_setting_panel(ui, "阅读主题");
    ui->roller = lv_roller_create(ui->body);
    lv_roller_set_options(ui->roller, "白昼\n夜间\n护眼", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui->roller, 3);
    lv_roller_set_selected(ui->roller, (uint32_t)ui->state->theme, LV_ANIM_OFF);
    lv_label_set_text(ui->status, "KEY1/2 切换  KEY3/4 返回菜单");
    ui->view = VIEW_THEME;
}

static void update_brightness(ebook_ui_t *ui)
{
    lv_slider_set_value(ui->slider, ui->state->brightness, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui->value_label, "%d / 9", ui->state->brightness);
}

static void render_brightness(ebook_ui_t *ui)
{
    make_setting_panel(ui, "屏幕亮度");
    ui->value_label = lv_label_create(ui->body);
    ui->slider = lv_slider_create(ui->body);
    lv_obj_set_size(ui->slider, lv_pct(85), scaled(ui, 14));
    lv_slider_set_range(ui->slider, 1, 9);
    update_brightness(ui);
    lv_label_set_text(ui->status, "KEY1/2 调整  KEY3/4 返回菜单");
    ui->view = VIEW_BRIGHTNESS;
}

static int font_size_index(const ebook_ui_t *ui)
{
    int index = 0;
    while(index < FONT_SIZE_COUNT && font_sizes[index] != ui->state->font_px)
        index++;
    return index < FONT_SIZE_COUNT ? index : 0;
}

static void render_font(ebook_ui_t *ui)
{
    make_setting_panel(ui, "正文字号");
    ui->roller = lv_roller_create(ui->body);
    lv_roller_set_options(ui->roller, "18\n20\n22\n26", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(ui->roller, 3);
    lv_roller_set_selected(ui->roller, (uint32_t)font_size_index(ui), LV_ANIM_OFF);
    lv_label_set_text(ui->status, "KEY1/2 切换  KEY3/4 返回菜单");
    ui->view = VIEW_FONT;
}

static void render_exit_confirm(ebook_ui_t *ui)
{
    lv_obj_t *box;
    close_msgbox(ui);
    box = lv_msgbox_create(NULL);
    ui->msgbox = box;
    lv_obj_set_style_text_font(box, ui->ui_font, 0);
    lv_obj_set_width(box, ui->platform->width - scaled(ui, 40));
    lv_msgbox_add_title(box, "确认退出");
    lv_msgbox_add_text(box, "确定退出阅读？当前进度会自动保存。");
    lv_msgbox_add_footer_button(box, "退出 (KEY3)");
    lv_msgbox_add_footer_button(box, "返回 (KEY4)");
    lv_label_set_text(ui->status, "KEY3 确认退出  KEY4 返回阅读");
    ui->view = VIEW_EXIT_CONFIRM;
}

static void activate_menu(ebook_ui_t *ui)
{
    size_t old_offset = ui->document->pages[ui->document->current_page];
    switch(ui->menu_index) {
    case 0:
        ui->jump_page = ui->document->current_page;
        render_jump(ui);
        break;
    case 1:
        ebook_state_add_bookmark(ui->document, ui->state, old_offset);
        ebook_state_save(ui->document, ui->state);
        render_menu(ui);
        break;
    case 2:
        ui->selection = 0;
        render_bookmarks(ui, false);
        break;
    case 3:
        ui->selection = 0;
        render_bookmarks(ui, true);
        break;
    case 4: render_theme(ui); break;
    case 5: render_brightness(ui); break;
    case 6: render_font(ui); break;
    default: render_reader(ui); break;
    }
}

ebook_ui_t *ebook_ui_create(ebook_platform_t *platform, ebook_document_t *document,
                            ebook_state_t *state)
{
    ebook_ui_t *ui = calloc(1, sizeof(*ui));
    int margin, status_h;
    if(!ui) return NULL;
    ui->platform = platform;
    ui->document = document;
    ui->state = state;
    margin = scaled(ui, 12);
    status_h = scaled(ui, 34);

    lv_style_init(&ui->style_item);
    lv_style_set_bg_opa(&ui->style_item, LV_OPA_COVER);
    lv_style_set_radius(&ui->style_item, scaled(ui, 6));
    lv_style_set_pad_ver(&ui->style_item, scaled(ui, 8));
    lv_style_init(&ui->style_item_focused);
    lv_style_set_bg_color(&ui->style_item_focused, lv_color_hex(0x2F6FED));
    lv_style_set_text_color(&ui->style_item_focused, lv_color_hex(0xFFFFFF));

    ui->status = lv_label_create(lv_screen_active());
    lv_obj_set_size(ui->status, platform->width, status_h);
    lv_obj_align(ui->status, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(ui->status, scaled(ui, 5), 0);
    lv_label_set_long_mode(ui->status, LV_LABEL_LONG_DOT);

    ui->body = lv_obj_create(lv_screen_active());
    lv_obj_set_size(ui->body, platform->width - margin * 2,
                    platform->height - status_h - margin * 2);
    lv_obj_align(ui->body, LV_ALIGN_TOP_MID, 0, margin);
    lv_obj_set_style_bg_opa(ui->body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui->body, 0, 0);
    lv_obj_set_style_pad_all(ui->body, 0, 0);

    ui->ui_font = load_font(scaled(ui, 20));
    if(ui->ui_font) {
        lv_obj_set_style_text_font(lv_screen_active(), ui->ui_font, 0);
        lv_obj_set_style_text_font(lv_layer_top(), ui->ui_font, 0);
    }
    if(ui->ui_font && rebuild_font(ui, state->position)) {
        apply_theme(ui);
        ebook_platform_set_brightness(state->brightness);
        render_reader(ui);
    } else {
        lv_obj_t *label = lv_label_create(ui->body);
        lv_label_set_text(label, "无法加载中文字体 SourceHanSansSC-Regular.otf");
        ui->should_exit = true;
    }
    return ui;
}

void ebook_ui_handle_key(ebook_ui_t *ui, ebook_key_t key)
{
    if(key == EBOOK_KEY_NONE) return;
    if(ui->view == VIEW_READER) {
        if(key == EBOOK_KEY_PREV && ui->document->current_page > 0)
            ui->document->current_page--;
        else if(key == EBOOK_KEY_NEXT &&
                ui->document->current_page + 1 < ui->document->page_count)
            ui->document->current_page++;
        else if(key == EBOOK_KEY_ENTER) {
            ui->menu_index = 0;
            render_menu(ui);
            return;
        } else if(key == EBOOK_KEY_BACK) {
            render_exit_confirm(ui);
            return;
        }
        render_reader(ui);
    } else if(ui->view == VIEW_MENU) {
        if(key == EBOOK_KEY_PREV) {
            ui->menu_index = (ui->menu_index + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
            select_list_item(ui, ui->menu_index);
        } else if(key == EBOOK_KEY_NEXT) {
            ui->menu_index = (ui->menu_index + 1) % MENU_ITEM_COUNT;
            select_list_item(ui, ui->menu_index);
        } else if(key == EBOOK_KEY_ENTER) {
            activate_menu(ui);
        } else if(key == EBOOK_KEY_BACK) {
            render_reader(ui);
        }
    } else if(ui->view == VIEW_JUMP) {
        if(key == EBOOK_KEY_PREV) {
            ui->jump_page = ui->jump_page > 0 ? ui->jump_page - 1 : 0;
            update_jump(ui);
        } else if(key == EBOOK_KEY_NEXT) {
            ui->jump_page++;
            if(ui->jump_page >= ui->document->page_count)
                ui->jump_page = ui->document->page_count - 1;
            update_jump(ui);
        } else if(key == EBOOK_KEY_ENTER) {
            ui->document->current_page = ui->jump_page;
            render_reader(ui);
        } else if(key == EBOOK_KEY_BACK) {
            render_menu(ui);
        }
    } else if(ui->view == VIEW_BOOKMARK_GOTO || ui->view == VIEW_BOOKMARK_DELETE) {
        bool deleting = ui->view == VIEW_BOOKMARK_DELETE;
        int count = ui->state->bookmark_count;
        if(count > 0 && key == EBOOK_KEY_PREV) {
            ui->selection = (ui->selection + count - 1) % count;
            select_list_item(ui, ui->selection);
        } else if(count > 0 && key == EBOOK_KEY_NEXT) {
            ui->selection = (ui->selection + 1) % count;
            select_list_item(ui, ui->selection);
        } else if(count > 0 && key == EBOOK_KEY_ENTER) {
            if(!deleting) {
                ui->document->current_page = ebook_page_for_offset(
                    ui->document, ui->state->bookmarks[ui->selection].offset);
                render_reader(ui);
                return;
            }
            ebook_state_delete_bookmark(ui->state, ui->selection);
            if(ui->selection >= ui->state->bookmark_count)
                ui->selection = ui->state->bookmark_count - 1;
            if(ui->selection < 0) ui->selection = 0;
            ebook_state_save(ui->document, ui->state);
            render_bookmarks(ui, true);
        } else if(key == EBOOK_KEY_BACK) {
            render_menu(ui);
        }
    } else if(ui->view == VIEW_THEME) {
        if(key == EBOOK_KEY_PREV)
            ui->state->theme = (ebook_theme_t)((ui->state->theme + 2) % 3);
        else if(key == EBOOK_KEY_NEXT)
            ui->state->theme = (ebook_theme_t)((ui->state->theme + 1) % 3);
        else if(key == EBOOK_KEY_ENTER || key == EBOOK_KEY_BACK) {
            ebook_state_save(ui->document, ui->state);
            render_menu(ui);
            return;
        } else {
            return;
        }
        lv_roller_set_selected(ui->roller, (uint32_t)ui->state->theme, LV_ANIM_OFF);
        apply_theme(ui);
        ebook_state_save(ui->document, ui->state);
    } else if(ui->view == VIEW_BRIGHTNESS) {
        if(key == EBOOK_KEY_PREV && ui->state->brightness > 1)
            ui->state->brightness--;
        else if(key == EBOOK_KEY_NEXT && ui->state->brightness < 9)
            ui->state->brightness++;
        else if(key == EBOOK_KEY_ENTER || key == EBOOK_KEY_BACK) {
            ebook_state_save(ui->document, ui->state);
            render_menu(ui);
            return;
        } else {
            return;
        }
        ebook_platform_set_brightness(ui->state->brightness);
        ebook_state_save(ui->document, ui->state);
        update_brightness(ui);
    } else if(ui->view == VIEW_FONT) {
        int index = font_size_index(ui);
        if(key == EBOOK_KEY_PREV)
            index = (index + FONT_SIZE_COUNT - 1) % FONT_SIZE_COUNT;
        else if(key == EBOOK_KEY_NEXT)
            index = (index + 1) % FONT_SIZE_COUNT;
        else if(key == EBOOK_KEY_ENTER || key == EBOOK_KEY_BACK) {
            ebook_state_save(ui->document, ui->state);
            render_menu(ui);
            return;
        } else {
            return;
        }
        {
            size_t old_offset = ui->document->pages[ui->document->current_page];
            ui->state->font_px = font_sizes[index];
            rebuild_font(ui, old_offset);
            ebook_state_save(ui->document, ui->state);
        }
        lv_roller_set_selected(ui->roller, (uint32_t)index, LV_ANIM_OFF);
    } else if(ui->view == VIEW_EXIT_CONFIRM) {
        if(key == EBOOK_KEY_ENTER) {
            ui->should_exit = true;
        } else if(key == EBOOK_KEY_BACK) {
            close_msgbox(ui);
            render_reader(ui);
        }
    }
}

bool ebook_ui_should_exit(const ebook_ui_t *ui) { return ui->should_exit; }

void ebook_ui_save_position(ebook_ui_t *ui)
{
    ui->state->position = ui->document->pages[ui->document->current_page];
    ebook_state_save(ui->document, ui->state);
}

void ebook_ui_destroy(ebook_ui_t *ui)
{
    if(!ui) return;
    close_msgbox(ui);
    lv_style_reset(&ui->style_item);
    lv_style_reset(&ui->style_item_focused);
    if(ui->font) lv_freetype_font_delete(ui->font);
    if(ui->ui_font) lv_freetype_font_delete(ui->ui_font);
    free(ui);
}
