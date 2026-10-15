#define _DEFAULT_SOURCE

#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

int camera_open(camera_t *cam, const char *device_path)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    unsigned int i;
    const char *path = device_path != NULL ? device_path : CAMERA_DEVICE_PATH;

    if (cam == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;

    cam->fd = open(path, O_RDWR | O_NONBLOCK);
    if (cam->fd < 0) {
        perror("camera open");
        return -1;
    }

    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "camera: device is not a capture device\n");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAMERA_WIDTH;
    fmt.fmt.pix.height = CAMERA_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT MJPEG");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr,
                "camera: driver did not accept MJPEG (got fourcc %u)\n",
                fmt.fmt.pix.pixelformat);
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;

    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        perror("VIDIOC_REQBUFS");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    cam->buffer_count = req.count;
    if (cam->buffer_count > 4) {
        cam->buffer_count = 4;
    }

    for (i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            camera_close(cam);
            return -1;
        }
        cam->buffer_lengths[i] = buf.length;
        cam->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, cam->fd, buf.m.offset);
        if (cam->buffers[i] == MAP_FAILED) {
            perror("mmap camera buffer");
            cam->buffers[i] = NULL;
            camera_close(cam);
            return -1;
        }
    }

    cam->initialized = 1;
    printf("camera: opened %s %ux%u MJPEG, %u buffers\n", path, cam->width,
           cam->height, cam->buffer_count);
    return 0;
}

int camera_start(camera_t *cam)
{
    enum v4l2_buf_type type;
    unsigned int i;

    if (cam == NULL || !cam->initialized) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}

int camera_stop(camera_t *cam)
{
    enum v4l2_buf_type type;

    if (cam == NULL || !cam->initialized) {
        return 0;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("VIDIOC_STREAMOFF");
        return -1;
    }
    return 0;
}

void camera_close(camera_t *cam)
{
    unsigned int i;

    if (cam == NULL) {
        return;
    }
    if (cam->initialized) {
        camera_stop(cam);
    }
    for (i = 0; i < cam->buffer_count; i++) {
        if (cam->buffers[i] != NULL && cam->buffers[i] != MAP_FAILED) {
            munmap(cam->buffers[i], cam->buffer_lengths[i]);
            cam->buffers[i] = NULL;
        }
    }
    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
    cam->initialized = 0;
    cam->buffer_count = 0;
}

int camera_capture_jpeg(camera_t *cam, unsigned char **out, size_t *out_len)
{
    struct v4l2_buffer buf;
    fd_set fds;
    struct timeval tv;
    int ready;
    unsigned char *copy;

    if (cam == NULL || !cam->initialized || out == NULL || out_len == NULL) {
        errno = EINVAL;
        return -1;
    }

    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ready = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0) {
        if (ready == 0) {
            errno = ETIMEDOUT;
        }
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("VIDIOC_DQBUF");
        return -1;
    }

    if (buf.index >= cam->buffer_count || buf.bytesused == 0) {
        xioctl(cam->fd, VIDIOC_QBUF, &buf);
        errno = EIO;
        return -1;
    }

    copy = malloc(buf.bytesused);
    if (copy == NULL) {
        xioctl(cam->fd, VIDIOC_QBUF, &buf);
        return -1;
    }
    memcpy(copy, cam->buffers[buf.index], buf.bytesused);
    *out = copy;
    *out_len = buf.bytesused;

    if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("VIDIOC_QBUF requeue");
        free(copy);
        *out = NULL;
        *out_len = 0;
        return -1;
    }
    return 0;
}
