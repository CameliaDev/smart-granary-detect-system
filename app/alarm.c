#include "alarm.h"

#include "LED.h"
#include "buzzer.h"
#include "fan.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

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
        if (st->supply_voltage_v < VOLTAGE_SAFE_MIN_V ||
            st->supply_voltage_v > VOLTAGE_SAFE_MAX_V) {
            st->e01 = 1;
            n += (size_t)snprintf(text + n, sizeof(text) - n,
                                  "E01 supply fault U=%.2fV; ",
                                  st->supply_voltage_v);
        }
        if (st->temperature_c > TEMP_SAFE_MAX_C ||
            st->humidity_rh > HUMIDITY_SAFE_MAX_RH) {
            st->e02 = 1;
            n += (size_t)snprintf(text + n, sizeof(text) - n,
                                  "E02 climate T=%.1fC RH=%.1f%%; ",
                                  st->temperature_c, st->humidity_rh);
        }
    }

    if (st->pir_detected || st->ai_intrusion) {
        st->e03 = 1;
        n += (size_t)snprintf(text + n, sizeof(text) - n, "E03 intrusion; ");
    }
    if (st->ai_mold) {
        st->e04 = 1;
        n += (size_t)snprintf(text + n, sizeof(text) - n, "E04 mold; ");
    }
    if (st->flame_detected || st->ai_collapse || st->ai_fire) {
        st->e05 = 1;
        n += (size_t)snprintf(
            text + n, sizeof(text) - n, "E05 collapse/fire; ");
    }

    if (n == 0) {
        snprintf(text, sizeof(text), "OK");
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
        /* Alternate LED1-3 */
        led1 = (phase % 3 == 0);
        led2 = (phase % 3 == 1);
        led3 = (phase % 3 == 2);
        if (!st->buzzer_muted) {
            buzz_hz = BUZZER_HIGH_FREQ_HZ;
        }
    } else {
        if (st->e01) {
            led1 = 1;
            if (!st->buzzer_muted) {
                /* Low-freq intermittent: 400ms on / 400ms off */
                if ((tick_ms / 400U) % 2U == 0U) {
                    buzz_hz = BUZZER_LOW_FREQ_HZ;
                }
            }
        }
        if (st->e02) {
            led2 = 1;
            fan_pwm = (int)FAN_ALARM_PWM;
        }
    }

    /* E02 fan also allowed alongside severe if climate bad */
    if (st->e02 && fan_pwm == 0) {
        fan_pwm = (int)FAN_ALARM_PWM;
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
