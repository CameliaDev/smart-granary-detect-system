#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>

int snapshot_ensure_dir(const char *dir);
int snapshot_save_jpeg(const char *dir, const unsigned char *jpeg, size_t len,
                       const char *tag);

#endif
