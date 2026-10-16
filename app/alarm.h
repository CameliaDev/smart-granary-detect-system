#ifndef ALARM_H
#define ALARM_H

#include "state.h"

#ifndef VOLTAGE_SAFE_MIN_V
#define VOLTAGE_SAFE_MIN_V 4.8f
#endif
#ifndef VOLTAGE_SAFE_MAX_V
#define VOLTAGE_SAFE_MAX_V 5.2f
#endif
#ifndef TEMP_SAFE_MAX_C
#define TEMP_SAFE_MAX_C 40.0f
#endif
#ifndef HUMIDITY_SAFE_MAX_RH
#define HUMIDITY_SAFE_MAX_RH 65.0f
#endif

#ifndef BASE_CURRENT_A
#define BASE_CURRENT_A 0.10f
#endif
#ifndef FAN_CURRENT_A
#define FAN_CURRENT_A 0.15f
#endif
#ifndef BUZZER_CURRENT_A
#define BUZZER_CURRENT_A 0.08f
#endif
#ifndef LED_BLINK_CURRENT_A
#define LED_BLINK_CURRENT_A 0.05f
#endif

#ifndef FAN_ALARM_PWM
#define FAN_ALARM_PWM 200U
#endif

#ifndef BUZZER_LOW_FREQ_HZ
#define BUZZER_LOW_FREQ_HZ 800U
#endif
#ifndef BUZZER_HIGH_FREQ_HZ
#define BUZZER_HIGH_FREQ_HZ 2500U
#endif

void alarm_evaluate(app_state_t *st);
void alarm_apply_actuators(app_state_t *st, unsigned int tick_ms);
void alarm_shutdown_actuators(void);

#endif
