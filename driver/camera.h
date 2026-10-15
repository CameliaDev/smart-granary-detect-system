#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>

#ifndef CAMERA_DEVICE_PATH
#define CAMERA_DEVICE_PATH "/dev/video0"
#endif

#ifndef CAMERA_WIDTH
#define CAMERA_WIDTH 640
#endif

#ifndef CAMERA_HEIGHT
#define CAMERA_HEIGHT 480
#endif

typedef struct {
    int fd;
    int initialized;
    unsigned int width;
    unsigned int height;
    void *buffers[4];
    size_t buffer_lengths[4];
    unsigned int buffer_count;
} camera_t;

int camera_open(camera_t *cam, const char *device_path);
int camera_start(camera_t *cam);
int camera_stop(camera_t *cam);
void camera_close(camera_t *cam);

/**
 * Capture one MJPEG frame into *out (malloc'd). Caller frees with free().
 * Returns 0 on success, -1 on failure.
 */
int camera_capture_jpeg(camera_t *cam, unsigned char **out, size_t *out_len);

#endif
