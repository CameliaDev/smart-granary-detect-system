/*
 * main.c —— 程序入口：初始化应用并创建全部工作线程
 *
 * 线程清单（见 app.h 的 APP_THREAD_*）：
 *   0 sensor  传感器轮询 + 告警判定 + 显示刷新
 *   1 key     按键扫描（KEY1 启停 / KEY2 抓拍+AI / KEY3 静音）
 *   2 camera  V4L2 采集 + 快照落盘 + AI 请求
 *   3 alarm   执行机构控制（LED / 蜂鸣器 / 风扇）
 *   4 ai      多模态大模型分析（条件变量唤醒）
 *   5 http    局域网网页推流 + 状态 JSON
 *   6 watchdog 线程看门狗（心跳超时则退出，交由 systemd 重启）
 */

#include "app.h"
#include "http_server.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    pthread_t threads[APP_THREAD_COUNT];
    void *(*const thread_functions[APP_THREAD_COUNT])(void *) = {
        app_sensor_thread,
        app_key_thread,
        app_camera_thread,
        app_alarm_thread,
        app_ai_thread,
        http_server_thread,
        app_watchdog_thread,
    };
    size_t created_count = 0;
    size_t i;
    int result;
    int exit_status = 0;

    if (app_init() < 0) {
        fprintf(stderr, "app_init failed\n");
        return 1;
    }

    for (i = 0; i < APP_THREAD_COUNT; i++) {
        result = pthread_create(&threads[i], NULL, thread_functions[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(result));
            app_request_stop();
            exit_status = 1;
            break;
        }
        created_count++;
    }

    for (i = 0; i < created_count; i++) {
        result = pthread_join(threads[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to join thread: %s\n", strerror(result));
            exit_status = 1;
        }
    }

    app_shutdown();
    return exit_status;
}
