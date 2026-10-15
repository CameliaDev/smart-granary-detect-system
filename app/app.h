#ifndef APP_H
#define APP_H

#include <stddef.h>

#include <linux/input.h> /* KEY_1/KEY_2/KEY_3 等按键码默认值 */

/*
 * 线程槽位（看门狗心跳索引）。
 * 数量须小于 state.h 的 STATE_THREAD_SLOTS。
 */
enum {
    APP_THREAD_SENSOR = 0,
    APP_THREAD_KEY,
    APP_THREAD_CAMERA,
    APP_THREAD_ALARM,
    APP_THREAD_AI,
    APP_THREAD_HTTP,
    APP_THREAD_WATCHDOG,
    APP_THREAD_COUNT
};

/* 任务周期默认值 (ms)，均可用环境变量覆盖（见 state.c config_load） */
#ifndef SENSOR_PERIOD_MS
#define SENSOR_PERIOD_MS 200U
#endif
#ifndef CAMERA_PERIOD_MS
#define CAMERA_PERIOD_MS 200U
#endif
#ifndef AI_PERIOD_MS
#define AI_PERIOD_MS 15000U
#endif
#ifndef DISPLAY_PERIOD_MS
#define DISPLAY_PERIOD_MS 2000U
#endif
#ifndef ALARM_PERIOD_MS
#define ALARM_PERIOD_MS 50U
#endif

/* 摄像头打开失败后的重连间隔 (ms) */
#ifndef CAMERA_RETRY_MS
#define CAMERA_RETRY_MS 2000U
#endif

/* 看门狗：线程心跳超过该秒数未更新视为卡死 */
#ifndef WATCHDOG_STALL_S
#define WATCHDOG_STALL_S 30
#endif
#ifndef WATCHDOG_PERIOD_MS
#define WATCHDOG_PERIOD_MS 5000U
#endif

/* 按键码（编译期覆盖：-DGRANARY_KEY1_CODE=...） */
#ifndef GRANARY_KEY1_CODE
#define GRANARY_KEY1_CODE KEY_1
#endif
#ifndef GRANARY_KEY2_CODE
#define GRANARY_KEY2_CODE KEY_2
#endif
#ifndef GRANARY_KEY3_CODE
#define GRANARY_KEY3_CODE KEY_3
#endif

void app_request_stop(void);
int app_init(void);
void app_shutdown(void);

void *app_sensor_thread(void *argument);
void *app_key_thread(void *argument);
void *app_camera_thread(void *argument);
void *app_alarm_thread(void *argument);
void *app_ai_thread(void *argument);
void *app_watchdog_thread(void *argument);
/* http_server_thread declared in http_server.h */

#endif
