#ifndef FILE_H
#define FILE_H

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif
/// 写字符串到文件
static inline int write_file(const char *path, const char *buf)
{
    int fd;
    size_t remaining;
    const char *cursor;
    ssize_t written;

    if (path == NULL || buf == NULL) {
        errno = EINVAL;
        perror("write_file");
        return -1;
    }

    fd = open(path, O_WRONLY | O_SYNC);
    if (fd < 0) {
        perror("open write failed");
        return -1;
    }

    cursor = buf;
    remaining = strlen(buf);
    while (remaining > 0) {
        written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            if (written == 0) {
                errno = EIO;
            }
            perror("write failed");
            close(fd);
            return -1;
        }
        cursor += written;
        remaining -= (size_t)written;
    }

    if (close(fd) < 0) {
        perror("close write file failed");
        return -1;
    }
    return 0;
}

/// 读文件里的字符串
static inline int read_file(const char *path, char *buf, int len)
{
    int fd;
    ssize_t ret;

    if (path == NULL || buf == NULL || len <= 0) {
        errno = EINVAL;
        perror("read_file");
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open read failed");
        return -1;
    }

    do {
        ret = read(fd, buf, (size_t)len - 1);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        perror("read failed");
        close(fd);
        return -1;
    }

    buf[ret] = '\0';
    if (close(fd) < 0) {
        perror("close read file failed");
        return -1;
    }
    return ret;
}

/// 在 /sys/bus/iio/devices/iio:deviceN 里找含有指定文件的设备目录
static inline int find_iio_device(const char *marker_file, char *dir, int dirlen)
{
    char path[160];
    int i;

    if (!marker_file || !dir || dirlen <= 0) {
        return -1;
    }

    for (i = 0; i < 16; i++) {
        int path_length = snprintf(path, sizeof(path),
                                   "/sys/bus/iio/devices/iio:device%d/%s",
                                   i, marker_file);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            return -1;
        }
        if (access(path, R_OK) == 0) {
            path_length = snprintf(dir, (size_t)dirlen,
                                   "/sys/bus/iio/devices/iio:device%d", i);
            if (path_length < 0 || path_length >= dirlen) {
                return -1;
            }
            return 0;
        }
    }

    return -1;
}
#ifdef __cplusplus
}

#endif

#endif
