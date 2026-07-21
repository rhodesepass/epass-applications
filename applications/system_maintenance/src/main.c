#include "port/platform.h"
#include "ui/ui.h"

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
    maintenance_platform_t platform;
    maintenance_ui_t *ui;
} maintenance_app_t;

static bool tick(void *userdata)
{
    maintenance_app_t *app = userdata;

    if(stopping || maintenance_ui_should_exit(app->ui)) return false;
    maintenance_ui_handle_key(app->ui,
                              maintenance_platform_read_key(&app->platform));
    maintenance_ui_tick(app->ui);
    uint32_t wait = lv_timer_handler();
    if(wait < 2U) wait = 2U;
    if(wait > 20U) wait = 20U;
    hal_idle(wait);
    return true;
}

int main(void)
{
    static maintenance_app_t app;
    int result = 1;

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if(!maintenance_platform_init(&app.platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        return 1;
    }
    /* FreeType 由 lv_init() 内部初始化（LV_USE_FREETYPE），无需手动调用 */
    app.ui = maintenance_ui_create(&app.platform);
    if(!app.ui) {
        fprintf(stderr, "failed to create maintenance UI\n");
        goto cleanup;
    }

    hal_run(tick, &app);

    result = 0;
    maintenance_ui_destroy(app.ui);

cleanup:
    maintenance_platform_destroy(&app.platform);
    return result;
}
