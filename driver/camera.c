#define _DEFAULT_SOURCE
#define _GNU_SOURCE

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

#define TJE_IMPLEMENTATION
#include "tiny_jpeg.h"

#ifndef V4L2_CAP_DEVICE_CAPS
#define V4L2_CAP_DEVICE_CAPS 0x80000000
#endif

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

static unsigned int camera_effective_caps(const struct v4l2_capability *cap)
{
    if (cap->capabilities & V4L2_CAP_DEVICE_CAPS) {
        return cap->device_caps;
    }
    return cap->capabilities;
}

static const char *fourcc_name(unsigned int fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
    return out;
}

static int clamp_u8(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

static void yuyv_to_rgb(const unsigned char *yuyv, unsigned char *rgb,
                        unsigned int width, unsigned int height)
{
    unsigned int x, y;

    for (y = 0; y < height; y++) {
        for (x = 0; x + 1 < width; x += 2) {
            size_t i = (size_t)(y * width + x) * 2U;
            int y0 = yuyv[i + 0];
            int u = yuyv[i + 1];
            int y1 = yuyv[i + 2];
            int v = yuyv[i + 3];
            int c0 = y0 - 16;
            int c1 = y1 - 16;
            int d = u - 128;
            int e = v - 128;
            size_t o = (size_t)(y * width + x) * 3U;

            rgb[o + 0] = (unsigned char)clamp_u8((298 * c0 + 409 * e + 128) >> 8);
            rgb[o + 1] = (unsigned char)clamp_u8((298 * c0 - 100 * d - 208 * e + 128) >> 8);
            rgb[o + 2] = (unsigned char)clamp_u8((298 * c0 + 516 * d + 128) >> 8);
            rgb[o + 3] = (unsigned char)clamp_u8((298 * c1 + 409 * e + 128) >> 8);
            rgb[o + 4] = (unsigned char)clamp_u8((298 * c1 - 100 * d - 208 * e + 128) >> 8);
            rgb[o + 5] = (unsigned char)clamp_u8((298 * c1 + 516 * d + 128) >> 8);
        }
    }
}

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} jpeg_grow_buf_t;

static void jpeg_grow_write(void *context, void *data, int size)
{
    jpeg_grow_buf_t *buf = (jpeg_grow_buf_t *)context;
    size_t need;

    if (buf == NULL || data == NULL || size <= 0) {
        return;
    }
    need = buf->len + (size_t)size;
    if (need > buf->cap) {
        size_t ncap = buf->cap ? buf->cap : 4096U;
        unsigned char *nd;

        while (ncap < need) {
            ncap *= 2U;
        }
        nd = realloc(buf->data, ncap);
        if (nd == NULL) {
            return;
        }
        buf->data = nd;
        buf->cap = ncap;
    }
    memcpy(buf->data + buf->len, data, (size_t)size);
    buf->len += (size_t)size;
}

static int encode_yuyv_jpeg(const unsigned char *yuyv, unsigned int width,
                            unsigned int height, unsigned char **out,
                            size_t *out_len)
{
    unsigned char *rgb;
    jpeg_grow_buf_t jbuf;
    int ok;

    if (yuyv == NULL || out == NULL || out_len == NULL || width == 0 ||
        height == 0 || (width % 2U) != 0U) {
        errno = EINVAL;
        return -1;
    }

    rgb = malloc((size_t)width * (size_t)height * 3U);
    if (rgb == NULL) {
        return -1;
    }
    yuyv_to_rgb(yuyv, rgb, width, height);

    memset(&jbuf, 0, sizeof(jbuf));
    ok = tje_encode_with_func(jpeg_grow_write, &jbuf, 2 /* quality */,
                              (int)width, (int)height, 3, rgb);
    free(rgb);
    if (!ok || jbuf.data == NULL || jbuf.len == 0) {
        free(jbuf.data);
        errno = EIO;
        return -1;
    }
    *out = jbuf.data;
    *out_len = jbuf.len;
    return 0;
}

static int try_set_format(int fd, unsigned int fourcc, unsigned int *width,
                          unsigned int *height, unsigned int *sizeimage)
{
    struct v4l2_format fmt;
    char name[5];

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = *width;
    fmt.fmt.pix.height = *height;
    fmt.fmt.pix.pixelformat = fourcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "camera: VIDIOC_S_FMT %s failed: %s\n",
                fourcc_name(fourcc, name), strerror(errno));
        return -1;
    }
    if (fmt.fmt.pix.pixelformat != fourcc) {
        fprintf(stderr, "camera: driver rejected %s (got %s)\n",
                fourcc_name(fourcc, name),
                fourcc_name(fmt.fmt.pix.pixelformat, name));
        return -1;
    }
    *width = fmt.fmt.pix.width;
    *height = fmt.fmt.pix.height;
    if (sizeimage != NULL) {
        *sizeimage = fmt.fmt.pix.sizeimage;
    }
    return 0;
}

static void camera_unmap(camera_t *cam)
{
    unsigned int i;

    for (i = 0; i < cam->buffer_count; i++) {
        if (cam->buffers[i] != NULL && cam->buffers[i] != MAP_FAILED) {
            munmap(cam->buffers[i], cam->buffer_lengths[i]);
        }
        cam->buffers[i] = NULL;
        cam->buffer_lengths[i] = 0;
    }
    cam->buffer_count = 0;
}

static int setup_mmap(camera_t *cam)
{
    struct v4l2_requestbuffers req;
    unsigned int i;

    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "camera: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        return -1;
    }
    if (req.count < 2) {
        fprintf(stderr, "camera: not enough mmap buffers (%u)\n", req.count);
        return -1;
    }

    cam->buffer_count = req.count > 4 ? 4 : req.count;
    for (i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr,
                    "camera: VIDIOC_QUERYBUF[%u] failed: %s (sizeof v4l2_buffer=%zu)\n",
                    i, strerror(errno), sizeof(struct v4l2_buffer));
            camera_unmap(cam);
            /* Release kernel buffers before READ fallback. */
            memset(&req, 0, sizeof(req));
            req.count = 0;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            xioctl(cam->fd, VIDIOC_REQBUFS, &req);
            return -1;
        }
        cam->buffer_lengths[i] = buf.length;
        cam->buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, cam->fd, buf.m.offset);
        if (cam->buffers[i] == MAP_FAILED) {
            perror("camera: mmap");
            cam->buffers[i] = NULL;
            camera_unmap(cam);
            memset(&req, 0, sizeof(req));
            req.count = 0;
            req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            req.memory = V4L2_MEMORY_MMAP;
            xioctl(cam->fd, VIDIOC_REQBUFS, &req);
            return -1;
        }
    }
    cam->io_method = CAMERA_IO_MMAP;
    return 0;
}

static int setup_read(camera_t *cam, unsigned int sizeimage)
{
    size_t need = sizeimage;

    if (need == 0) {
        need = (size_t)cam->width * (size_t)cam->height * 2U;
        if (cam->pixelformat == V4L2_PIX_FMT_MJPEG) {
            need = (size_t)cam->width * (size_t)cam->height;
        }
    }
    /* Extra headroom for MJPEG variable size / partial reads. */
    if (need < 65536U) {
        need = 65536U;
    }
    cam->read_buf = malloc(need);
    if (cam->read_buf == NULL) {
        return -1;
    }
    cam->read_buf_size = need;
    cam->io_method = CAMERA_IO_READ;
    cam->buffer_count = 0;
    return 0;
}

static int open_one_device(camera_t *cam, const char *path)
{
    struct v4l2_capability cap;
    unsigned int caps;
    unsigned int width = CAMERA_WIDTH;
    unsigned int height = CAMERA_HEIGHT;
    unsigned int sizeimage = 0;
    unsigned int format = 0;
    char fcc[5];

    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;
    snprintf(cam->device_path, sizeof(cam->device_path), "%s", path);

    cam->fd = open(path, O_RDWR | O_NONBLOCK);
    if (cam->fd < 0) {
        fprintf(stderr, "camera: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "camera: QUERYCAP %s: %s\n", path, strerror(errno));
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    caps = camera_effective_caps(&cap);
    fprintf(stderr,
            "camera: %s driver=%s card=%s caps=0x%x\n", path, cap.driver,
            cap.card, caps);

    if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "camera: %s is not VIDEO_CAPTURE (skip)\n", path);
        close(cam->fd);
        cam->fd = -1;
        errno = ENODEV;
        return -1;
    }

    /* Prefer MJPEG; fall back to YUYV (common on UVC). */
    if (try_set_format(cam->fd, V4L2_PIX_FMT_MJPEG, &width, &height,
                       &sizeimage) == 0) {
        format = V4L2_PIX_FMT_MJPEG;
    } else {
        width = CAMERA_WIDTH;
        height = CAMERA_HEIGHT;
        sizeimage = 0;
        if (try_set_format(cam->fd, V4L2_PIX_FMT_YUYV, &width, &height,
                           &sizeimage) == 0) {
            format = V4L2_PIX_FMT_YUYV;
        } else {
            close(cam->fd);
            cam->fd = -1;
            errno = EINVAL;
            return -1;
        }
    }

    cam->width = width;
    cam->height = height;
    cam->pixelformat = format;

    if ((caps & V4L2_CAP_STREAMING) != 0 && setup_mmap(cam) == 0) {
        /* mmap path ready */
    } else if ((caps & V4L2_CAP_READWRITE) != 0 &&
               setup_read(cam, sizeimage) == 0) {
        fprintf(stderr, "camera: using READ I/O on %s\n", path);
    } else if (setup_read(cam, sizeimage) == 0) {
        /* Some UVC nodes advertise STREAMING but READ still works; try anyway. */
        fprintf(stderr, "camera: forcing READ I/O on %s\n", path);
    } else {
        close(cam->fd);
        cam->fd = -1;
        errno = EIO;
        return -1;
    }

    cam->initialized = 1;
    printf("camera: opened %s %ux%u %s via %s\n", path, cam->width, cam->height,
           fourcc_name(format, fcc),
           cam->io_method == CAMERA_IO_MMAP ? "mmap" : "read");
    return 0;
}

int camera_open(camera_t *cam, const char *device_path)
{
    const char *candidates[4];
    unsigned int n = 0;
    unsigned int i;
    const char *env;

    if (cam == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (device_path != NULL && device_path[0] != '\0') {
        candidates[n++] = device_path;
    } else {
        env = getenv("CAMERA_DEVICE");
        if (env != NULL && env[0] != '\0') {
            candidates[n++] = env;
        } else {
            /* video1 = UVC capture on FS-MP1A; video2 = meta; video0 = dcmi */
            candidates[n++] = "/dev/video1";
            candidates[n++] = "/dev/video2";
            candidates[n++] = "/dev/video0";
        }
    }

    for (i = 0; i < n; i++) {
        if (open_one_device(cam, candidates[i]) == 0) {
            return 0;
        }
    }

    fprintf(stderr, "camera: no usable capture device\n");
    errno = ENODEV;
    return -1;
}

int camera_start(camera_t *cam)
{
    enum v4l2_buf_type type;
    unsigned int i;

    if (cam == NULL || !cam->initialized) {
        errno = EINVAL;
        return -1;
    }

    if (cam->io_method == CAMERA_IO_READ) {
        return 0;
    }

    for (i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("camera: VIDIOC_QBUF");
            return -1;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("camera: VIDIOC_STREAMON");
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
    if (cam->io_method == CAMERA_IO_READ) {
        return 0;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cam->fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("camera: VIDIOC_STREAMOFF");
        return -1;
    }
    return 0;
}

void camera_close(camera_t *cam)
{
    if (cam == NULL) {
        return;
    }
    if (cam->initialized) {
        camera_stop(cam);
    }
    camera_unmap(cam);
    free(cam->read_buf);
    cam->read_buf = NULL;
    cam->read_buf_size = 0;
    if (cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }
    cam->initialized = 0;
}

static int wait_readable(int fd, int timeout_sec)
{
    fd_set fds;
    struct timeval tv;
    int ready;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    ready = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ready <= 0) {
        if (ready == 0) {
            errno = ETIMEDOUT;
        }
        return -1;
    }
    return 0;
}

static int frame_to_jpeg(camera_t *cam, const unsigned char *data, size_t len,
                         unsigned char **out, size_t *out_len)
{
    unsigned char *copy;

    if (cam->pixelformat == V4L2_PIX_FMT_MJPEG) {
        if (len < 2 || data[0] != 0xff || data[1] != 0xd8) {
            /* Some drivers prepend headers; still accept if large enough. */
            if (len < 100) {
                errno = EIO;
                return -1;
            }
        }
        copy = malloc(len);
        if (copy == NULL) {
            return -1;
        }
        memcpy(copy, data, len);
        *out = copy;
        *out_len = len;
        return 0;
    }

    if (cam->pixelformat == V4L2_PIX_FMT_YUYV) {
        size_t expect = (size_t)cam->width * (size_t)cam->height * 2U;
        if (len < expect) {
            errno = EIO;
            return -1;
        }
        return encode_yuyv_jpeg(data, cam->width, cam->height, out, out_len);
    }

    errno = EINVAL;
    return -1;
}

static int capture_mmap(camera_t *cam, unsigned char **out, size_t *out_len)
{
    struct v4l2_buffer buf;
    int rc;

    if (wait_readable(cam->fd, 2) < 0) {
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("camera: VIDIOC_DQBUF");
        return -1;
    }

    if (buf.index >= cam->buffer_count || buf.bytesused == 0) {
        xioctl(cam->fd, VIDIOC_QBUF, &buf);
        errno = EIO;
        return -1;
    }

    rc = frame_to_jpeg(cam, (const unsigned char *)cam->buffers[buf.index],
                       buf.bytesused, out, out_len);
    if (xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("camera: VIDIOC_QBUF requeue");
        if (rc == 0) {
            free(*out);
            *out = NULL;
            *out_len = 0;
        }
        return -1;
    }
    return rc;
}

static int capture_read(camera_t *cam, unsigned char **out, size_t *out_len)
{
    ssize_t n;

    if (cam->read_buf == NULL || cam->read_buf_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (wait_readable(cam->fd, 2) < 0) {
        return -1;
    }

    do {
        n = read(cam->fd, cam->read_buf, cam->read_buf_size);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            errno = ETIMEDOUT;
        }
        perror("camera: read");
        return -1;
    }
    if (n == 0) {
        errno = EIO;
        return -1;
    }

    return frame_to_jpeg(cam, cam->read_buf, (size_t)n, out, out_len);
}

int camera_capture_jpeg(camera_t *cam, unsigned char **out, size_t *out_len)
{
    if (cam == NULL || !cam->initialized || out == NULL || out_len == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    *out_len = 0;

    if (cam->io_method == CAMERA_IO_MMAP) {
        return capture_mmap(cam, out, out_len);
    }
    return capture_read(cam, out, out_len);
}
