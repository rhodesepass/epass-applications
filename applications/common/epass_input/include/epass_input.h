#pragma once

/*
 * evdev 导航键采集: 不再写死 /dev/input/event0, 而是扫描整个 /dev/input,
 * 用 EVIOCGBIT 过滤出真正带 KEY_0..4 的设备并全部打开。多设备并存时
 * (例如同时挂了矩阵键盘和某个只上报 BTN_* 的触摸/传感器节点) 只保留
 * 我们关心的那些, 且不会漏掉排在 event0 之后的键盘。
 */

#define EPASS_INPUT_MAX_FDS 16

/*
 * 扫描 /dev/input/event*, 打开所有具备 KEY_0..4 能力的设备。
 * fds 由调用方提供 (容量 max_fds), 填入非阻塞只读 fd, 返回打开的数量 (0 表示没找到)。
 */
int epass_input_open_nav(int *fds, int max_fds);

/* 关闭 epass_input_open_nav 填入的所有 fd, 并把它们置 -1。 */
void epass_input_close(int *fds, int count);
