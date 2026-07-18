#pragma once

#include "port/platform.h"
#include "player/mediaplayer.h"
#include <stdbool.h>

typedef struct vp_ui vp_ui_t;

vp_ui_t *vp_ui_create(vp_platform_t *platform, mediaplayer_t *mp);
void vp_ui_destroy(vp_ui_t *ui);

void vp_ui_handle_key_event(vp_ui_t *ui, const vp_key_event_t *ev);
/* 主循环每轮调用：长按/连按步进/scrub 自动提交/自动隐藏 */
void vp_ui_tick(vp_ui_t *ui);
bool vp_ui_should_exit(vp_ui_t *ui);
