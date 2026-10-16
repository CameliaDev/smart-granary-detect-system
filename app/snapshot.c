/*
 * snapshot.c —— 告警快照保存与管理
 *
 * 存储布局：<dir>/YYYY-MM-DD/snap_HHMMSS_<tag>.jpg
 *   tag: alarm=告警自动抓拍 / manual=按键手动抓拍
 *
 * 保留策略：每次保存后清理修改时间超过 GRANARY_SNAPSHOT_KEEP_DAYS
 * （默认 7 天，0 表示不清理）的文件，并删除清空后的日期目录，
 * 防止长期运行撑满存储。
 */

#define _POSIX_C_SOURCE 200809L

#include "snapshot.h"

#include "log.h"
#include "state.h"

#include <dirent.h>
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

/* 删除超过 keep_days 天的快照文件与空日期目录；keep_days<=0 时不清理 */
static void prune_old_snapshots(const char *root, int keep_days)
{
    DIR *root_dir;
    struct dirent *entry;
    time_t cutoff;

    if (root == NULL || keep_days <= 0) {
        return;
    }
    cutoff = time(NULL) - (time_t)keep_days * 86400;

    root_dir = opendir(root);
    if (root_dir == NULL) {
        return;
    }

    while ((entry = readdir(root_dir)) != NULL) {
        char sub_path[512];
        struct stat st;
        DIR *sub_dir;
        struct dirent *file;
        int empty = 1;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(sub_path, sizeof(sub_path), "%s/%s", root,
                     entry->d_name) >= (int)sizeof(sub_path)) {
            continue;
        }
        if (stat(sub_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            /* 根目录下的散落文件也一并清理 */
            if (stat(sub_path, &st) == 0 && S_ISREG(st.st_mode) &&
                st.st_mtime < cutoff) {
                unlink(sub_path);
            }
            continue;
        }

        /* 日期子目录：清理过期文件，空了则删除目录 */
        sub_dir = opendir(sub_path);
        if (sub_dir == NULL) {
            continue;
        }
        while ((file = readdir(sub_dir)) != NULL) {
            char file_path[640];
            struct stat fst;

            if (strcmp(file->d_name, ".") == 0 ||
                strcmp(file->d_name, "..") == 0) {
                continue;
            }
            if (snprintf(file_path, sizeof(file_path), "%s/%s", sub_path,
                         file->d_name) >= (int)sizeof(file_path)) {
                empty = 0;
                continue;
            }
            if (stat(file_path, &fst) == 0 && S_ISREG(fst.st_mode)) {
                if (fst.st_mtime < cutoff) {
                    unlink(file_path);
                } else {
                    empty = 0;
                }
            }
        }
        closedir(sub_dir);
        if (empty) {
            rmdir(sub_path);
        }
    }
    closedir(root_dir);
}

int snapshot_save_jpeg(const char *dir, const unsigned char *jpeg, size_t len,
                       const char *tag)
{
    char date_dir[320];
    char path[640];
    time_t now;
    struct tm tm_now;
    FILE *fp;
    size_t written;
    int keep_days = 7;

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

    /* 保留天数取自运行期配置（与 dir 无关，安全读取） */
    {
        app_state_t *st = state_get();
        pthread_mutex_lock(&st->lock);
        keep_days = st->cfg.snapshot_keep_days;
        pthread_mutex_unlock(&st->lock);
    }

    now = time(NULL);
    localtime_r(&now, &tm_now);

    /* 按日期建子目录：dir/YYYY-MM-DD/ */
    if (snprintf(date_dir, sizeof(date_dir), "%s/%04d-%02d-%02d", dir,
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday) >=
        (int)sizeof(date_dir)) {
        return -1;
    }
    if (snapshot_ensure_dir(date_dir) < 0) {
        return -1;
    }

    if (snprintf(path, sizeof(path), "%s/snap_%02d%02d%02d_%s.jpg", date_dir,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                 tag != NULL ? tag : "alarm") >= (int)sizeof(path)) {
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("fopen snapshot");
        return -1;
    }
    written = fwrite(jpeg, 1, len, fp);
    fclose(fp);
    if (written != len) {
        log_error("snapshot: short write to %s", path);
        return -1;
    }
    log_info("snapshot saved: %s (%zu bytes)", path, len);

    /* 保存成功后执行保留策略清理 */
    prune_old_snapshots(dir, keep_days);
    return 0;
}
