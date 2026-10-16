#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>

/* 确保目录存在（不存在则创建，含多级父目录由调用方保证） */
int snapshot_ensure_dir(const char *dir);

/*
 * 保存一帧 JPEG 到 dir 下按日期分的子目录：
 *   dir/YYYY-MM-DD/snap_HHMMSS_<tag>.jpg
 * 保存成功后按 dir 的保留天数（GRANARY_SNAPSHOT_KEEP_DAYS，0=不清理）
 * 自动清理过期文件与空目录。失败返回 -1（调用方回退 /tmp）。
 */
int snapshot_save_jpeg(const char *dir, const unsigned char *jpeg, size_t len,
                       const char *tag);

#endif
