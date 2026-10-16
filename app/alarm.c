/*
 * alarm.c —— 告警判定与执行机构控制
 *
 * 判定规则（对照功能说明 PDF 4.3）：
 *   E01 供电电压异常：U ∉ [cfg.voltage_min_v, cfg.voltage_max_v]
 *   E02 温湿度超限  ：T > cfg.temp_max_c 或 RH > cfg.rh_max_rh
 *   E03 人员闯入    ：人体红外传感器 或 AI 识别到人员
 *   E04 粮情异常    ：AI 识别到 霉变结块/粮面塌陷/虫害堆积/杂物遗留
 *   E05 明火隐患    ：火焰传感器 或 AI 识别到明火/烟雾
 *
 * 执行规则：
 *   E01        → LED1 常亮 + 蜂鸣器低频间断（鸣/静各 400ms）
 *   E02        → LED2 常亮 + 风扇通风（PWM 可配）
 *   E03/E04/E05→ LED1~LED3 交替闪烁 + 蜂鸣器高频持续
 *   巡检暂停    → 全部执行机构归位（待机）
 *
 * 阈值/频率/占空比均来自 st->cfg（环境变量可覆盖，见 state.c）。
 */

#include "alarm.h"

#include "LED.h"
#include "buzzer.h"
#include "fan.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 安全追加格式化文本。
 * 修复原实现 "n += snprintf(...)" 累加返回值在超长时导致
 * size_t 下溢（sizeof(text) - n 变成巨大值）的缓冲区溢出风险：
 * 本函数保证返回值 n 始终钳位在 [0, size-1]。
 */
static size_t appendf(char *buf, size_t size, size_t n, const char *fmt, ...)
{
    va_list ap;
    int written;

    if (buf == NULL || size == 0) {
        return 0;
    }
    if (n >= size - 1) {
        return size - 1;
    }
    va_start(ap, fmt);
    written = vsnprintf(buf + n, size - n, fmt, ap);
    va_end(ap);
    if (written < 0) {
        return n;
    }
    n += (size_t)written;
    if (n >= size) {
        n = size - 1;
    }
    return n;
}

void alarm_evaluate(app_state_t *st)
{
    char text[STATE_ALARM_TEXT_MAX];
    char overlay[STATE_OVERLAY_MAX];
    size_t n = 0;

    if (st == NULL) {
        return;
    }

    st->e01 = 0;
    st->e02 = 0;
    st->e03 = 0;
    st->e04 = 0;
    st->e05 = 0;
    text[0] = '\0';
    overlay[0] = '\0';

    if (st->sensors_valid) {
        if (st->supply_voltage_v < st->cfg.voltage_min_v ||
            st->supply_voltage_v > st->cfg.voltage_max_v) {
            st->e01 = 1;
            n = appendf(text, sizeof(text), n, "E01 supply fault U=%.2fV; ",
                        st->supply_voltage_v);
        }
        if (st->temperature_c > st->cfg.temp_max_c ||
            st->humidity_rh > st->cfg.rh_max_rh) {
            st->e02 = 1;
            n = appendf(text, sizeof(text), n, "E02 climate T=%.1fC RH=%.1f%%; ",
                        st->temperature_c, st->humidity_rh);
        }
    }

    if (st->pir_detected || st->ai_intrusion) {
        st->e03 = 1;
        n = appendf(text, sizeof(text), n, "E03 intrusion; ");
    }
    /* E04 粮情异常：霉变结块 / 粮面塌陷 / 虫害堆积 / 杂物遗留 */
    if (st->ai_mold || st->ai_collapse || st->ai_pest || st->ai_debris) {
        st->e04 = 1;
        n = appendf(text, sizeof(text), n, "E04 grain anomaly; ");
    }
    /* E05 明火隐患：火焰传感器 或 AI 识别明火/烟雾 */
    if (st->flame_detected || st->ai_fire) {
        st->e05 = 1;
        n = appendf(text, sizeof(text), n, "E05 fire; ");
    }

    if (n == 0) {
        snprintf(text, sizeof(text), "OK");
    }

    /* 告警文本发生变化时记录一条历史（含恢复为 OK） */
    if (strcmp(text, st->alarm_text) != 0) {
        state_push_alarm_history_locked(text);
    }

    {
        char ai_short[160];
        strncpy(ai_short, st->ai_text, sizeof(ai_short) - 1);
        ai_short[sizeof(ai_short) - 1] = '\0';
        snprintf(overlay, sizeof(overlay),
                 "U=%.2fV I=%.2fA P=%.2fW T=%.1fC RH=%.1f%% Lux=%.0f | %s | AI:%s",
                 st->supply_voltage_v, st->current_a, st->power_w,
                 st->temperature_c, st->humidity_rh, st->illuminance, text,
                 ai_short);
    }

    strncpy(st->alarm_text, text, sizeof(st->alarm_text) - 1);
    st->alarm_text[sizeof(st->alarm_text) - 1] = '\0';
    strncpy(st->overlay_text, overlay, sizeof(st->overlay_text) - 1);
    st->overlay_text[sizeof(st->overlay_text) - 1] = '\0';
}

void alarm_apply_actuators(app_state_t *st, unsigned int tick_ms)
{
    static int last_led1 = -1;
    static int last_led2 = -1;
    static int last_led3 = -1;
    static int last_fan = -1;
    static unsigned int last_buzz = UINT_MAX;
    int severe;
    int fan_pwm = 0;
    int led1 = 0;
    int led2 = 0;
    int led3 = 0;
    int leds_blink = 0;
    unsigned int buzz_hz = BUZZER_OFF_FREQUENCY;
    int phase;

    if (st == NULL) {
        return;
    }

    /* 巡检暂停：全部执行机构待机 */
    if (!st->inspection_running) {
        if (last_led1 != 0) {
            ledcontrol(LED_DEVICE_ID_1, LED_OFF);
            last_led1 = 0;
        }
        if (last_led2 != 0) {
            ledcontrol(LED_DEVICE_ID_2, LED_OFF);
            last_led2 = 0;
        }
        if (last_led3 != 0) {
            ledcontrol(LED_DEVICE_ID_3, LED_OFF);
            last_led3 = 0;
        }
        if (last_fan != 0) {
            fan_set(0U);
            last_fan = 0;
        }
        if (last_buzz != BUZZER_OFF_FREQUENCY) {
            buzzer_set(BUZZER_OFF_FREQUENCY);
            last_buzz = BUZZER_OFF_FREQUENCY;
        }
        st->fan_on = 0;
        st->buzzer_sounding = 0;
        st->leds_all_blinking = 0;
        return;
    }

    severe = st->e03 || st->e04 || st->e05;
    phase = (int)((tick_ms / 200U) % 6U);

    if (severe) {
        leds_blink = 1;
        /* LED1~LED3 交替闪烁：每个相位轮流点亮一颗 */
        led1 = (phase % 3 == 0);
        led2 = (phase % 3 == 1);
        led3 = (phase % 3 == 2);
        if (!st->buzzer_muted) {
            buzz_hz = st->cfg.buzzer_high_hz;
        }
    } else {
        if (st->e01) {
            led1 = 1;
            if (!st->buzzer_muted) {
                /* 低频间断鸣响：400ms 鸣叫 / 400ms 静音 */
                if ((tick_ms / 400U) % 2U == 0U) {
                    buzz_hz = st->cfg.buzzer_low_hz;
                }
            }
        }
        if (st->e02) {
            led2 = 1;
            fan_pwm = (int)st->cfg.fan_alarm_pwm;
        }
    }

    /* E02 风扇在高级别告警并存时同样保持通风 */
    if (st->e02 && fan_pwm == 0) {
        fan_pwm = (int)st->cfg.fan_alarm_pwm;
    }

    if (led1 != last_led1) {
        ledcontrol(LED_DEVICE_ID_1, led1 ? LED_ON : LED_OFF);
        last_led1 = led1;
    }
    if (led2 != last_led2) {
        ledcontrol(LED_DEVICE_ID_2, led2 ? LED_ON : LED_OFF);
        last_led2 = led2;
    }
    if (led3 != last_led3) {
        ledcontrol(LED_DEVICE_ID_3, led3 ? LED_ON : LED_OFF);
        last_led3 = led3;
    }
    if (fan_pwm != last_fan) {
        fan_set((unsigned int)fan_pwm);
        last_fan = fan_pwm;
    }
    if (buzz_hz != last_buzz) {
        buzzer_set(buzz_hz);
        last_buzz = buzz_hz;
    }

    st->fan_on = fan_pwm > 0;
    st->buzzer_sounding = buzz_hz > 0;
    st->leds_all_blinking = leds_blink;
}

void alarm_shutdown_actuators(void)
{
    ledcontrol(LED_DEVICE_ID_1, LED_OFF);
    ledcontrol(LED_DEVICE_ID_2, LED_OFF);
    ledcontrol(LED_DEVICE_ID_3, LED_OFF);
    fan_set(0U);
    buzzer_set(BUZZER_OFF_FREQUENCY);
}
