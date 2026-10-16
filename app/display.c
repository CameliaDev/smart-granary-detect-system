/*
 * display.c —— 终端显示（LCD 替代方案）
 *
 * 将传感器实时数据、AI 识别结果与告警状态打印到 stdout，
 * 同时这些数据也通过 HTTP /status.json 叠加到局域网网页画面。
 * 传感器读取失败时对应项显示 ERR（见 state.h 的 *_valid 标志）。
 */

#include "display.h"

#include <stdio.h>

/** Print sensor + AI + alarms to stdout (LCD substitute). */
void display_refresh(const app_state_t *st)
{
    printf("\033[2J\033[H");   /* 清屏，模拟 LCD 整屏刷新 */
    printf("======== 智能粮仓检测系统 ========\n");
    printf("巡检状态 : %s\n", st->inspection_running ? "运行中" : "已暂停");
    printf("供电电压 : %.2f V (ADC %.2f V, x2)\n", st->supply_voltage_v,
           st->adc_voltage_v);
    printf("负载电流 : %.2f A\n", st->current_a);
    printf("实时功率 : %.2f W (P=U*I)\n", st->power_w);
    printf("仓内温度 : %s%.1f ℃\n", st->temp_valid ? "" : "ERR ",
           st->temperature_c);
    printf("仓内湿度 : %s%.1f %%RH\n", st->hum_valid ? "" : "ERR ",
           st->humidity_rh);
    printf("光照强度 : %s%.0f lux\n", st->lux_valid ? "" : "ERR ",
           st->illuminance);
    printf("人体红外 : %s\n", st->pir_detected ? "有人闯入!" : "无人");
    printf("火焰传感 : %s\n", st->flame_detected ? "检测到明火!" : "正常");
    printf("AI 识别  : %s\n", st->ai_text);
    printf("告警状态 : %s\n", st->alarm_text);
    printf("快照数量 : %llu\n", (unsigned long long)st->snapshot_count);
}

/** Optional framebuffer stub — currently no-op if FB unavailable. */
void display_framebuffer_stub(const app_state_t *st)
{
    /* TODO: 接入 framebuffer(/dev/fb0) 或 SPI LCD 驱动时在此实现 */
    (void)st;
}
