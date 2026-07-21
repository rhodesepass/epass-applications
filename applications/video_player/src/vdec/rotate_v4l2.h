/*
 * cedrus-rotate（VE SDROT 单元）V4L2 m2m 封装，规范见
 * refresh_new_200s/cedrus-rotate-usage.md。
 *
 * 输入输出都是 ST12（32x32 tiled NV12，即 cedrus 解码节点的原生输出）。
 * OUTPUT 端零拷贝吃解码 CAPTURE 的 dmabuf；CAPTURE 端 MMAP 分配并
 * EXPBUF 导出，直接喂 hal_display_import_dmabuf_fb 上视频层。
 *
 * 会话 = 一组 (角度, 尺寸, buffer 池)；切角度必须 stop 后重新 start。
 * 同步单帧：一次只有一对 buffer 在硬件里，与解码共用 VE 由内核排队。
 */

#ifndef _VDEC_ROTATE_V4L2_H_
#define _VDEC_ROTATE_V4L2_H_

#include <stdbool.h>
#include <stdint.h>

#define ROT_MAX_CAP_BUFS 8

struct rot_cap_buf {
	void *map;
	unsigned int length;
	int dmabuf_fd;
};

struct rot_ctx {
	int fd;			/* 设备 fd，rot_open 后跨会话保持 */
	bool streaming;

	int angle;		/* 0/90/180/270 */
	unsigned int src_width;	/* OUTPUT 端 = 视频可见宽高（非 32 对齐值） */
	unsigned int src_height;
	unsigned int src_sizeimage;

	/* CAPTURE（旋转后）布局，S_FMT 回读；喂 DRM 导入用 */
	unsigned int cap_width;
	unsigned int cap_height;
	unsigned int cap_bytesperline;
	unsigned int cap_sizeimage;
	unsigned int cap_uv_offset;

	unsigned int cap_count;
	struct rot_cap_buf cap[ROT_MAX_CAP_BUFS];
};

/* 扫 /dev/video* 认 cap.card == "cedrus-rotate"。成功返回 0。 */
int rot_open(struct rot_ctx *r);
void rot_close(struct rot_ctx *r);

/*
 * 建会话直到 STREAMON：格式（90/270 时 CAPTURE 宽高对调）、角度、
 * OUTPUT 端 1 个 DMABUF 槽、CAPTURE 端 cap_count 个 MMAP+EXPBUF buffer。
 */
int rot_session_start(struct rot_ctx *r, int angle,
		      unsigned int width, unsigned int height,
		      unsigned int cap_count);

/* STREAMOFF + 释放两端 buffer。fd 保持打开，可再 start。 */
int rot_session_stop(struct rot_ctx *r);

/*
 * 同步转一帧：src_dmabuf_fd（解码 CAPTURE 的 EXPBUF fd）→ cap[cap_index]。
 * 返回 0 成功；1 = 硬件报错该帧不可用（流保持可用）；-1 = 会话故障，
 * 调用方应 stop/start 重建。
 */
int rot_run(struct rot_ctx *r, int src_dmabuf_fd, int cap_index);

#endif /* _VDEC_ROTATE_V4L2_H_ */
