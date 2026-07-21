#include "port/platform.h"
#include "ui/ui.h"

#include "hal_run.h"
#include <lvgl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* 与 rootfs S01app 约定: 首次开机教程完成后 touch 此文件, 之后跳过自动启动 */
#define QUICK_START_DONE "/etc/.quick_start_done"

static volatile sig_atomic_t stopping;

static void stop_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

typedef struct {
    tutorial_platform_t platform;
    tutorial_ui_t *ui;
} tutorial_app_t;

static bool tick(void *userdata)
{
    tutorial_app_t *app = userdata;

    if(stopping || tutorial_ui_should_exit(app->ui)) return false;
    tutorial_ui_handle_key(app->ui, tutorial_platform_read_key(&app->platform));
    tutorial_ui_tick(app->ui);
    uint32_t wait = lv_timer_handler();
    if(wait < 2U) wait = 2U;
    if(wait > 20U) wait = 20U;
    hal_idle(wait);
    return true;
}

int main(void)
{
    static tutorial_app_t app;
    int result = 1;

    /* 每次手动/自动进入教程都清掉“已跑过”标记, 便于下次开机再次自动引导 */
    unlink(QUICK_START_DONE);

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if(!tutorial_platform_init(&app.platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        return 1;
    }
    /* FreeType 由 lv_init() 内部初始化（LV_USE_FREETYPE），无需手动调用 */
    app.ui = tutorial_ui_create(&app.platform);
    if(!app.ui) {
        fprintf(stderr, "failed to create tutorial UI\n");
        goto cleanup;
    }

    hal_run(tick, &app);

    result = 0;
    tutorial_ui_destroy(app.ui);

cleanup:
    tutorial_platform_destroy(&app.platform);
    return result;
}
