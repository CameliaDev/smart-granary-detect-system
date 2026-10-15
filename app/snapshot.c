#define _POSIX_C_SOURCE 200809L

#include "snapshot.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int snapshot_ensure_dir(const char *dir)
{
    struct stat st;

    if (dir == NULL || dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir snapshot dir");
        return -1;
    }
    return 0;
}

int snapshot_save_jpeg(const char *dir, const unsigned char *jpeg, size_t len,
                       const char *tag)
{
    char path[384];
    time_t now;
    struct tm tm_now;
    FILE *fp;
    size_t written;

    if (jpeg == NULL || len == 0) {
        return -1;
    }
    if (snapshot_ensure_dir(dir) < 0) {
        /* fallback */
        dir = "/tmp/granary-snapshots";
        if (snapshot_ensure_dir(dir) < 0) {
            return -1;
        }
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);
    snprintf(path, sizeof(path),
             "%s/snap_%04d%02d%02d_%02d%02d%02d_%s.jpg", dir,
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             tag != NULL ? tag : "alarm");

    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("fopen snapshot");
        return -1;
    }
    written = fwrite(jpeg, 1, len, fp);
    fclose(fp);
    if (written != len) {
        fprintf(stderr, "snapshot: short write to %s\n", path);
        return -1;
    }
    printf("snapshot saved: %s (%zu bytes)\n", path, len);
    return 0;
}
