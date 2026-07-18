#include "port/platform.h"
#include "player/mediaplayer.h"
#include "ui/ui.h"

#include <lvgl.h>
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

static bool is_mp4(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".mp4");
}

int main(int argc, char **argv)
{
    vp_platform_t platform;
    mediaplayer_t mp;
    vp_ui_t *ui = NULL;
    int result = 1;

    if(argc != 2 || !is_mp4(argv[1])) {
        fprintf(stderr, "usage: %s /path/to/video.mp4\n", argv[0]);
        return 2;
    }
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);

    if(!vp_platform_init(&platform)) {
        fprintf(stderr, "failed to initialize DRM/LVGL/input\n");
        return 1;
    }
    if(mediaplayer_init(&mp, &platform.drm, platform.width, platform.height) < 0)
        goto cleanup_platform;
    if(mediaplayer_play_video(&mp, argv[1]) < 0) {
        fprintf(stderr, "failed to start playback: %s\n", argv[1]);
        goto cleanup_player;
    }

    ui = vp_ui_create(&platform, &mp);
    if(!ui) goto cleanup_player;

    while(!stopping && !vp_ui_should_exit(ui)) {
        vp_key_event_t ev;
        while(vp_platform_read_key_event(&platform, &ev))
            vp_ui_handle_key_event(ui, &ev);
        vp_ui_tick(ui);
        uint32_t wait = lv_timer_handler();
        if(wait < 2) wait = 2;
        if(wait > 20) wait = 20;
        usleep(wait * 1000);
    }
    result = 0;
    vp_ui_destroy(ui);
cleanup_player:
    mediaplayer_stop(&mp);
    mediaplayer_destroy(&mp);
cleanup_platform:
    vp_platform_destroy(&platform);
    return result;
}
