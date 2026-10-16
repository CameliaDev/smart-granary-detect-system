/*
 * log.c —— 简易日志模块实现
 */

#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static log_level_t log_level = LOG_LEVEL_INFO;
static FILE *log_file = NULL; /* NULL = 仅 stderr */

const char *log_level_name(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG:    return "DEBUG";
    case LOG_LEVEL_INFO:     return "INFO";
    case LOG_LEVEL_WARN:     return "WARN";
    case LOG_LEVEL_ERROR:    return "ERROR";
    case LOG_LEVEL_CRITICAL: return "CRITICAL";
    default:                 return "UNKNOWN";
    }
}

static log_level_t parse_level(const char *name, log_level_t def)
{
    if (name == NULL) {
        return def;
    }
    if (strcmp(name, "debug") == 0)   return LOG_LEVEL_DEBUG;
    if (strcmp(name, "info") == 0)    return LOG_LEVEL_INFO;
    if (strcmp(name, "warn") == 0)    return LOG_LEVEL_WARN;
    if (strcmp(name, "error") == 0)   return LOG_LEVEL_ERROR;
    if (strcmp(name, "critical") == 0) return LOG_LEVEL_CRITICAL;
    return def;
}

void log_init(const char *file_path)
{
    const char *level_name = getenv("GRANARY_LOG_LEVEL");
    const char *env_file = getenv("GRANARY_LOG_FILE");

    log_level = parse_level(level_name, LOG_LEVEL_INFO);

    /* 参数优先，其次环境变量 */
    if (file_path == NULL || file_path[0] == '\0') {
        file_path = env_file;
    }
    if (file_path != NULL && file_path[0] != '\0') {
        FILE *fp = fopen(file_path, "a");
        if (fp != NULL) {
            log_file = fp;
        } else {
            fprintf(stderr, "log: cannot open %s: %s\n", file_path,
                    strerror(errno));
        }
    }
}

void log_shutdown(void)
{
    pthread_mutex_lock(&log_mutex);
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

log_level_t log_set_level(log_level_t level)
{
    log_level_t old;

    pthread_mutex_lock(&log_mutex);
    old = log_level;
    log_level = level;
    pthread_mutex_unlock(&log_mutex);
    return old;
}

log_level_t log_get_level(void)
{
    log_level_t level;

    pthread_mutex_lock(&log_mutex);
    level = log_level;
    pthread_mutex_unlock(&log_mutex);
    return level;
}

void log_msg(log_level_t level, const char *fmt, ...)
{
    struct timespec ts;
    struct tm tm_now;
    char stamp[32];
    va_list ap;
    FILE *targets[2];
    int targets_count = 0;
    int i;

    if (level < log_get_level()) {
        return;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_now);
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
             tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
             (long)(ts.tv_nsec / 1000000L));

    pthread_mutex_lock(&log_mutex);
    targets[targets_count++] = stderr;
    if (log_file != NULL) {
        targets[targets_count++] = log_file;
    }

    for (i = 0; i < targets_count; i++) {
        fprintf(targets[i], "[%s] %-8s ", stamp, log_level_name(level));
        va_start(ap, fmt);
        vfprintf(targets[i], fmt, ap);
        va_end(ap);
        fputc('\n', targets[i]);
        fflush(targets[i]);
    }
    pthread_mutex_unlock(&log_mutex);
}
