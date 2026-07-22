#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "rotate_v4l2.h"
#include "utils/log.h"

#ifndef V4L2_PIX_FMT_SUNXI_TILED_NV12
#define V4L2_PIX_FMT_SUNXI_TILED_NV12 v4l2_fourcc('S', 'T', '1', '2')
#endif

#define ROT_ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))

int rot_open(struct rot_ctx *r)
{
	char path[32];
	struct v4l2_capability cap;
	int i, fd;

	memset(r, 0, sizeof(*r));
	r->fd = -1;

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		memset(&cap, 0, sizeof(cap));
		if (!ioctl(fd, VIDIOC_QUERYCAP, &cap) &&
		    !strcmp((char *)cap.card, "cedrus-rotate")) {
			log_info("cedrus-rotate at %s", path);
			r->fd = fd;
			return 0;
		}
		close(fd);
	}
	log_error("cedrus-rotate node not found");
	return -1;
}

void rot_close(struct rot_ctx *r)
{
	if (r->streaming)
		rot_session_stop(r);
	if (r->fd >= 0) {
		close(r->fd);
		r->fd = -1;
	}
}

static int rot_set_format(int fd, unsigned int type, unsigned int width,
			  unsigned int height, struct v4l2_format *out)
{
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_SUNXI_TILED_NV12;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		log_error("rot S_FMT type %u err: %s", type, strerror(errno));
		return -1;
	}
	if (out)
		*out = fmt;
	return 0;
}

static int rot_reqbufs(int fd, unsigned int type, unsigned int memory,
		       unsigned int count)
{
	struct v4l2_requestbuffers req;

	memset(&req, 0, sizeof(req));
	req.type = type;
	req.memory = memory;
	req.count = count;
	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		log_error("rot REQBUFS type %u count %u err: %s", type, count,
			  strerror(errno));
		return -1;
	}
	return 0;
}

static void rot_teardown_cap(struct rot_ctx *r)
{
	unsigned int i;

	for (i = 0; i < r->cap_count; i++) {
		if (r->cap[i].map && r->cap[i].map != MAP_FAILED)
			munmap(r->cap[i].map, r->cap[i].length);
		if (r->cap[i].dmabuf_fd >= 0)
			close(r->cap[i].dmabuf_fd);
		r->cap[i].map = NULL;
		r->cap[i].dmabuf_fd = -1;
	}
	r->cap_count = 0;
}

int rot_session_start(struct rot_ctx *r, int angle, bool vflip,
		      unsigned int width, unsigned int height,
		      unsigned int cap_count)
{
	struct v4l2_format fmt;
	struct v4l2_control ctrl;
	unsigned int cap_w, cap_h, h16;
	unsigned int i;
	int type;

	if (r->fd < 0 || r->streaming || cap_count > ROT_MAX_CAP_BUFS)
		return -1;
	if (angle != 0 && angle != 90 && angle != 180 && angle != 270)
		return -1;
	if (angle == 0 && !vflip)
		return -1;
	if (vflip && (angle == 90 || angle == 270)) {
		/* 转置 op6/7 硬件未验证，驱动 S_CTRL 会拒；调用方拆两趟 */
		log_error("rot vflip+%d unsupported", angle);
		return -1;
	}

	/* OUTPUT 传视频真实宽高：传 32 对齐值会连 padding 宏块一起转，画面偏移 */
	if (rot_set_format(r->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			   width, height, &fmt) < 0)
		return -1;
	r->src_width = width;
	r->src_height = height;
	r->src_sizeimage = fmt.fmt.pix.sizeimage;

	if (angle == 90 || angle == 270) {
		cap_w = height;
		cap_h = width;
	} else {
		cap_w = width;
		cap_h = height;
	}
	if (rot_set_format(r->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			   cap_w, cap_h, &fmt) < 0)
		return -1;
	r->cap_width = cap_w;
	r->cap_height = cap_h;
	r->cap_bytesperline = fmt.fmt.pix.bytesperline;
	r->cap_sizeimage = fmt.fmt.pix.sizeimage;
	/* luma 平面 = stride * ALIGN(ALIGN(h,16),32)，chroma 紧随（usage 文档布局） */
	h16 = ROT_ALIGN(cap_h, 16);
	r->cap_uv_offset = r->cap_bytesperline * ROT_ALIGN(h16, 32);

	/* ctrl 状态跨会话残留在 fd 上，组合检查按当前值算：先清镜像位，
	 * 否则上一会话的 vflip 会把本次 rotate=90 的 S_CTRL 顶成 -EINVAL */
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.id = V4L2_CID_HFLIP;
	ctrl.value = 0;
	ioctl(r->fd, VIDIOC_S_CTRL, &ctrl);
	ctrl.id = V4L2_CID_VFLIP;
	ctrl.value = 0;
	ioctl(r->fd, VIDIOC_S_CTRL, &ctrl);

	ctrl.id = V4L2_CID_ROTATE;
	ctrl.value = angle;
	if (ioctl(r->fd, VIDIOC_S_CTRL, &ctrl) < 0) {
		log_error("rot S_CTRL rotate=%d err: %s", angle, strerror(errno));
		return -1;
	}
	if (vflip) {
		ctrl.id = V4L2_CID_VFLIP;
		ctrl.value = 1;
		if (ioctl(r->fd, VIDIOC_S_CTRL, &ctrl) < 0) {
			log_error("rot S_CTRL vflip err: %s", strerror(errno));
			return -1;
		}
	}
	r->angle = angle;
	r->vflip = vflip;

	if (rot_reqbufs(r->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT,
			V4L2_MEMORY_DMABUF, 1) < 0)
		return -1;
	if (rot_reqbufs(r->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE,
			V4L2_MEMORY_MMAP, cap_count) < 0)
		goto err_out_bufs;

	r->cap_count = cap_count;
	for (i = 0; i < cap_count; i++) {
		r->cap[i].map = NULL;
		r->cap[i].dmabuf_fd = -1;
	}
	for (i = 0; i < cap_count; i++) {
		struct v4l2_buffer buf;
		struct v4l2_exportbuffer exp;

		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if (ioctl(r->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			log_error("rot QUERYBUF %u err: %s", i, strerror(errno));
			goto err_cap_bufs;
		}
		r->cap[i].length = buf.length;
		r->cap[i].map = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, r->fd, buf.m.offset);
		if (r->cap[i].map == MAP_FAILED) {
			log_error("rot mmap %u err: %s", i, strerror(errno));
			goto err_cap_bufs;
		}

		memset(&exp, 0, sizeof(exp));
		exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		exp.index = i;
		exp.flags = O_RDWR;
		if (ioctl(r->fd, VIDIOC_EXPBUF, &exp) < 0) {
			log_error("rot EXPBUF %u err: %s", i, strerror(errno));
			goto err_cap_bufs;
		}
		r->cap[i].dmabuf_fd = exp.fd;
	}

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(r->fd, VIDIOC_STREAMON, &type) < 0) {
		log_error("rot STREAMON out err: %s", strerror(errno));
		goto err_cap_bufs;
	}
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(r->fd, VIDIOC_STREAMON, &type) < 0) {
		log_error("rot STREAMON cap err: %s", strerror(errno));
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		ioctl(r->fd, VIDIOC_STREAMOFF, &type);
		goto err_cap_bufs;
	}
	r->streaming = true;

	log_info("rot session: %ux%u -> %ux%u angle=%d vflip=%d bufs=%u stride=%u size=%u",
		 width, height, cap_w, cap_h, angle, vflip, cap_count,
		 r->cap_bytesperline, r->cap_sizeimage);
	return 0;

err_cap_bufs:
	rot_teardown_cap(r);
	rot_reqbufs(r->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, 0);
err_out_bufs:
	rot_reqbufs(r->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, 0);
	return -1;
}

int rot_session_stop(struct rot_ctx *r)
{
	int type;

	if (r->fd < 0)
		return -1;
	if (r->streaming) {
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		ioctl(r->fd, VIDIOC_STREAMOFF, &type);
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(r->fd, VIDIOC_STREAMOFF, &type);
		r->streaming = false;
	}
	rot_teardown_cap(r);
	rot_reqbufs(r->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, 0);
	rot_reqbufs(r->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_DMABUF, 0);
	return 0;
}

int rot_run(struct rot_ctx *r, int src_dmabuf_fd, int cap_index)
{
	struct v4l2_buffer buf;
	bool frame_error = false;

	if (!r->streaming || cap_index < 0 ||
	    (unsigned int)cap_index >= r->cap_count)
		return -1;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.index = 0;
	buf.m.fd = src_dmabuf_fd;
	buf.bytesused = r->src_sizeimage;
	if (ioctl(r->fd, VIDIOC_QBUF, &buf) < 0) {
		log_error("rot QBUF out err: %s", strerror(errno));
		return -1;
	}

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = (unsigned int)cap_index;
	if (ioctl(r->fd, VIDIOC_QBUF, &buf) < 0) {
		log_error("rot QBUF cap err: %s", strerror(errno));
		return -1;
	}

	/* 阻塞到硬件转完（~1ms 级）。永久阻塞 = VE 中断丢失，上层重建会话 */
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(r->fd, VIDIOC_DQBUF, &buf) < 0) {
		log_error("rot DQBUF cap err: %s", strerror(errno));
		return -1;
	}
	if (buf.flags & V4L2_BUF_FLAG_ERROR)
		frame_error = true;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_DMABUF;
	if (ioctl(r->fd, VIDIOC_DQBUF, &buf) < 0) {
		log_error("rot DQBUF out err: %s", strerror(errno));
		return -1;
	}

	return frame_error ? 1 : 0;
}
