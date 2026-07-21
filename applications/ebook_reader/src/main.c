#include "port/platform.h"
#include "reader/reader.h"
#include "ui/ui.h"

#include "hal_run.h"
#include <lvgl.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static bool txt_path(const char *path)
{
    size_t length = strlen(path);
    return length >= 4 && path[length - 4] == '.' &&
        tolower((unsigned char)path[length - 3]) == 't' &&
        tolower((unsigned char)path[length - 2]) == 'x' &&
        tolower((unsigned char)path[length - 1]) == 't';
}

typedef struct {
    ebook_document_t document;
    ebook_state_t state;
    ebook_platform_t platform;
    ebook_ui_t *ui;
} ebook_app_t;

static bool tick(void *userdata)
{
    ebook_app_t *app = userdata;

    if(stopping || ebook_ui_should_exit(app->ui)) return false;
    ebook_ui_handle_key(app->ui, ebook_platform_read_key(&app->platform));
    uint32_t wait = lv_timer_handler();
    if(wait < 2) wait = 2;
    if(wait > 20) wait = 20;
    hal_idle(wait);
    return true;
}

int main(int argc, char **argv)
{
    static ebook_app_t app;
    int result = 1;
    if(argc != 2 || !txt_path(argv[1])) {
        fprintf(stderr, "usage: %s /absolute/path/book.txt\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if(!ebook_load_document(&app.document, argv[1])) {
        fprintf(stderr, "failed to load TXT: %s\n", argv[1]);
        return 1;
    }
    ebook_state_load(&app.document, &app.state);
    if(!ebook_platform_init(&app.platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        ebook_free_document(&app.document);
        return 1;
    }
    /* FreeType 由 lv_init() 内部初始化（LV_USE_FREETYPE），无需手动调用 */
    app.ui = ebook_ui_create(&app.platform, &app.document, &app.state);
    if(!app.ui) goto cleanup;

    hal_run(tick, &app);

    ebook_ui_save_position(app.ui);
    result = 0;
    ebook_ui_destroy(app.ui);
cleanup:
    ebook_platform_destroy(&app.platform);
    ebook_free_document(&app.document);
    return result;
}
