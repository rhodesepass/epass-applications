#include "port/platform.h"
#include "ui/ui.h"

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

int main(void)
{
    tutorial_platform_t platform;
    tutorial_ui_t *ui = NULL;
    int result = 1;

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if(!tutorial_platform_init(&platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        return 1;
    }
    /* FreeType 由 lv_init() 内部初始化（LV_USE_FREETYPE），无需手动调用 */
    ui = tutorial_ui_create(&platform);
    if(!ui) {
        fprintf(stderr, "failed to create tutorial UI\n");
        goto cleanup;
    }

    while(!stopping && !tutorial_ui_should_exit(ui)) {
        tutorial_ui_handle_key(ui, tutorial_platform_read_key(&platform));
        tutorial_ui_tick(ui);
        uint32_t wait = lv_timer_handler();
        if(wait < 2U) wait = 2U;
        if(wait > 20U) wait = 20U;
        usleep(wait * 1000U);
    }
    result = 0;
    tutorial_ui_destroy(ui);

cleanup:
    tutorial_platform_destroy(&platform);
    return result;
}
