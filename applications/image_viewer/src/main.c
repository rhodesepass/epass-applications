#include "port/platform.h"
#include "ui/ui.h"
#include "viewer/gallery.h"

#include "hal_run.h"
#include <lvgl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

typedef struct {
    iv_gallery_t gallery;
    iv_platform_t platform;
    iv_ui_t *ui;
} iv_app_t;

static bool tick(void *userdata)
{
    iv_app_t *app = userdata;

    if(stopping || iv_ui_should_exit(app->ui)) return false;
    iv_ui_handle_key(app->ui, iv_platform_read_key(&app->platform));
    uint32_t wait = lv_timer_handler();
    if(wait < 2) wait = 2;
    if(wait > 20) wait = 20;
    hal_idle(wait);
    return true;
}

int main(int argc, char **argv)
{
    static iv_app_t app;
    int result = 1;
    if(argc != 2) {
        fprintf(stderr, "usage: %s /path/to/image\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if(!iv_gallery_load(&app.gallery, argv[1])) {
        fprintf(stderr, "failed to enumerate images beside: %s\n", argv[1]);
        return 1;
    }
    if(!iv_platform_init(&app.platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        iv_gallery_free(&app.gallery);
        return 1;
    }
    app.ui = iv_ui_create(&app.platform, &app.gallery);
    if(!app.ui) goto cleanup;

    hal_run(tick, &app);

    result = 0;
    iv_ui_destroy(app.ui);
cleanup:
    iv_platform_destroy(&app.platform);
    iv_gallery_free(&app.gallery);
    return result;
}
