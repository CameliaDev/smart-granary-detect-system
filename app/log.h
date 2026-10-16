#ifndef LOG_H
#define LOG_H

/*
 * log.h —— 简易日志模块
 *
 * 特性：
 *   - 级别：DEBUG < INFO < WARN < ERROR < CRITICAL，可运行期过滤；
 *   - 输出：stderr；设置 GRANARY_LOG_FILE 后可同时落盘（追加）；
 *   - 级别过滤：GRANARY_LOG_LEVEL = debug|info|warn|error（默认 info）；
 *   - 格式：[YYYY-MM-DD HH:MM:SS.mmm] LEVEL 消息；
 *   - 线程安全（内部互斥锁）。
 *
 * 用法：
 *   log_info("sensor thread started, period=%ums", period_ms);
 *   log_error("open %s failed: %s", path, strerror(errno));
 */

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL
} log_level_t;

/* 读取环境变量并初始化（app_init 中调用）；file_path 为 NULL 时仅输出 stderr */
void log_init(const char *file_path);

/* 关闭日志文件（app_shutdown 中调用） */
void log_shutdown(void);

/* 设置级别过滤（返回原级别） */
log_level_t log_set_level(log_level_t level);

/* 获取当前级别 */
log_level_t log_get_level(void);

/* 输出一条日志（自动加时间戳与级别标签） */
void log_msg(log_level_t level, const char *fmt, ...);

/* 级别名称（"INFO" 等） */
const char *log_level_name(log_level_t level);

#define log_debug(...)    log_msg(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define log_info(...)     log_msg(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_warn(...)     log_msg(LOG_LEVEL_WARN, __VA_ARGS__)
#define log_error(...)    log_msg(LOG_LEVEL_ERROR, __VA_ARGS__)
#define log_critical(...) log_msg(LOG_LEVEL_CRITICAL, __VA_ARGS__)

#endif
