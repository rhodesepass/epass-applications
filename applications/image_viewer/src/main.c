#include "port/platform.h"
#include "ui/ui.h"
#include "viewer/gallery.h"

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

int main(int argc, char **argv)
{
    iv_gallery_t gallery;
    iv_platform_t platform;
    iv_ui_t *ui = NULL;
    int result = 1;
    if(argc != 2 || !iv_gallery_is_image(argv[1])) {
        fprintf(stderr, "usage: %s /path/to/image.{jpg,jpeg,png,bmp,gif}\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    if(!iv_gallery_load(&gallery, argv[1])) {
        fprintf(stderr, "failed to enumerate images beside: %s\n", argv[1]);
        return 1;
    }
    if(!iv_platform_init(&platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        iv_gallery_free(&gallery);
        return 1;
    }
    ui = iv_ui_create(&platform, &gallery);
    if(!ui) goto cleanup;
    while(!stopping && !iv_ui_should_exit(ui)) {
        iv_key_t key = iv_platform_read_key(&platform);
        iv_ui_handle_key(ui, key);
        uint32_t wait = lv_timer_handler();
        if(wait < 2) wait = 2;
        if(wait > 20) wait = 20;
        usleep(wait * 1000);
    }
    result = 0;
    iv_ui_destroy(ui);
cleanup:
    iv_platform_destroy(&platform);
    iv_gallery_free(&gallery);
    return result;
}
