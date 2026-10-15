#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>

#ifndef CAMERA_DEVICE_PATH
/* FS-MP1A UVC capture node (video0=dcmi ENODEV, video2=meta only). */
#define CAMERA_DEVICE_PATH "/dev/video1"
#endif

#ifndef CAMERA_WIDTH
#define CAMERA_WIDTH 640
#endif

#ifndef CAMERA_HEIGHT
#define CAMERA_HEIGHT 480
#endif

typedef enum {
    CAMERA_IO_MMAP = 0,
    CAMERA_IO_READ = 1
} camera_io_method_t;

typedef struct {
    int fd;
    int initialized;
    unsigned int width;
    unsigned int height;
    unsigned int pixelformat; /* V4L2_PIX_FMT_MJPEG or V4L2_PIX_FMT_YUYV */
    camera_io_method_t io_method;
    void *buffers[4];
    size_t buffer_lengths[4];
    unsigned int buffer_count;
    unsigned char *read_buf;
    size_t read_buf_size;
    char device_path[64];
} camera_t;

/**
 * Open and configure a capture device.
 * device_path: explicit path, or NULL to use CAMERA_DEVICE env then
 * /dev/video1, /dev/video2, /dev/video0 (skipping non-capture nodes).
 */
int camera_open(camera_t *cam, const char *device_path);
int camera_start(camera_t *cam);
int camera_stop(camera_t *cam);
void camera_close(camera_t *cam);

/**
 * Capture one JPEG frame into *out (malloc'd). Caller frees with free().
 * MJPEG devices return the frame as-is; YUYV is encoded to JPEG.
 * Returns 0 on success, -1 on failure.
 */
int camera_capture_jpeg(camera_t *cam, unsigned char **out, size_t *out_len);

#endif
