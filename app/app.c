#define _POSIX_C_SOURCE 200809L

#include "app.h"

#include "LED.h"
#include "buzzer.h"
#include "delay.h"
#include "fan.h"
#include "ht_sensor.h"
#include "light_sensor.h"
#include "voltage_sensor.h"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>

#define VOLTAGE_ALARM_THRESHOLD_MV 2200.0f
#define HUMIDITY_ALARM_THRESHOLD_PERCENT 60.0f
#define TEMPERATURE_FAN_THRESHOLD_C 30.0f
#define TEMPERATURE_FAN_PWM 200U
#define PROXIMITY_ALARM_THRESHOLD 100.0f
#define APP_POLL_INTERVAL_MS 200U

static volatile sig_atomic_t keep_running = 1;
static pthread_once_t signal_handlers_once = PTHREAD_ONCE_INIT;
static int signal_handlers_initialized;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static void initialize_signal_handlers(void)
{
    struct sigaction action = {0};

    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("Failed to install application signal handlers");
        signal_handlers_initialized = 0;
        return;
    }
    signal_handlers_initialized = 1;
}

static int app_prepare_thread(void)
{
    int result = pthread_once(&signal_handlers_once, initialize_signal_handlers);

    if (result != 0) {
        fprintf(stderr, "Failed to initialize application thread: %d\n",
                result);
        app_request_stop();
        return -1;
    }
    if (!signal_handlers_initialized) {
        app_request_stop();
        return -1;
    }
    return 0;
}

void app_request_stop(void)
{
    keep_running = 0;
}

static void update_voltage_indicator(void)
{
    float voltage_mv;
    LED_Status led_status = LED_OFF;

    if (voltage_sensor_read(&voltage_mv) < 0 || !isfinite(voltage_mv)) {
        fprintf(stderr, "Failed to read voltage sensor; turning LED 1 off\n");
    } else {
        printf("Voltage: %.2f mV (%.3f V)\n",
               voltage_mv, voltage_mv / 1000.0f);
        led_status = voltage_mv >= VOLTAGE_ALARM_THRESHOLD_MV
                         ? LED_ON
                         : LED_OFF;
        printf("Voltage threshold %.0f mV: LED 1 %s\n",
               VOLTAGE_ALARM_THRESHOLD_MV,
               led_status == LED_ON ? "on" : "off");
    }

    if (ledcontrol(LED_DEVICE_ID_1, led_status) < 0) {
        fprintf(stderr, "Failed to update LED 1\n");
    }
}

static void update_climate_outputs(void)
{
    float temperature = ht_read_temperature();
    float humidity = ht_read_humidity();
    int temperature_valid =
        isfinite(temperature) && temperature != HT_SENSOR_ERROR_VALUE;
    int humidity_valid =
        isfinite(humidity) && humidity != HT_SENSOR_ERROR_VALUE;
    LED_Status humidity_led = LED_OFF;
    unsigned int fan_pwm = 0U;

    if (temperature_valid) {
        printf("Temperature: %.2f C\n", temperature);
        if (temperature > TEMPERATURE_FAN_THRESHOLD_C) {
            fan_pwm = TEMPERATURE_FAN_PWM;
            printf("Temperature above %.1f C: fan PWM %u\n",
                   TEMPERATURE_FAN_THRESHOLD_C, fan_pwm);
        } else {
            printf("Temperature normal: fan off\n");
        }
    } else {
        fprintf(stderr, "Failed to read temperature; turning fan off\n");
    }

    if (humidity_valid) {
        printf("Humidity: %.2f %%RH\n", humidity);
        humidity_led = humidity > HUMIDITY_ALARM_THRESHOLD_PERCENT
                           ? LED_ON
                           : LED_OFF;
        printf("Humidity threshold %.1f %%RH: LED 2 %s\n",
               HUMIDITY_ALARM_THRESHOLD_PERCENT,
               humidity_led == LED_ON ? "on" : "off");
    } else {
        fprintf(stderr, "Failed to read humidity; turning LED 2 off\n");
    }

    if (fan_set(fan_pwm) < 0) {
        fprintf(stderr, "Failed to set fan PWM to %u\n", fan_pwm);
    }
    if (ledcontrol(LED_DEVICE_ID_2, humidity_led) < 0) {
        fprintf(stderr, "Failed to update LED 2\n");
    }
}

static void update_proximity_buzzer(void)
{
    float proximity = light_read_proximity();
    unsigned int frequency = BUZZER_OFF_FREQUENCY;

    if (!isfinite(proximity) || proximity < 0.0f) {
        fprintf(stderr,
                "Failed to read proximity sensor; turning buzzer off\n");
    } else {
        printf("Proximity: %.0f\n", proximity);
        if (proximity > PROXIMITY_ALARM_THRESHOLD) {
            frequency = 2000U;
            printf("Nearby object detected: buzzer on\n");
        } else {
            printf("No nearby object: buzzer off\n");
        }
    }

    if (buzzer_set(frequency) < 0) {
        fprintf(stderr, "Failed to set buzzer frequency to %u Hz\n",
                frequency);
    }
}

static void shutdown_voltage_output(void)
{
    if (ledcontrol(LED_DEVICE_ID_1, LED_OFF) < 0) {
        fprintf(stderr, "Failed to turn off LED 1 during shutdown\n");
    }
    if (ledcontrol(LED_DEVICE_ID_3, LED_OFF) < 0) {
        fprintf(stderr, "Failed to turn off LED 3 during shutdown\n");
    }
}

static void shutdown_climate_outputs(void)
{
    if (ledcontrol(LED_DEVICE_ID_2, LED_OFF) < 0) {
        fprintf(stderr, "Failed to turn off LED 2 during shutdown\n");
    }
    if (fan_set(0U) < 0) {
        fprintf(stderr, "Failed to turn off fan during shutdown\n");
    }
}

static void shutdown_proximity_output(void)
{
    if (buzzer_set(BUZZER_OFF_FREQUENCY) < 0) {
        fprintf(stderr, "Failed to turn off buzzer during shutdown\n");
    }
}

void *app_voltage_thread(void *argument)
{
    (void)argument;
    if (app_prepare_thread() < 0) {
        shutdown_voltage_output();
        return NULL;
    }

    if (ledcontrol(LED_DEVICE_ID_3, LED_OFF) < 0) {
        fprintf(stderr, "Failed to initialize LED 3\n");
    }
    printf("Voltage alarm threshold: %.0f mV (LED 1)\n",
           VOLTAGE_ALARM_THRESHOLD_MV);
    puts("Voltage monitoring thread started");
    while (keep_running) {
        update_voltage_indicator();
        delay_ms(APP_POLL_INTERVAL_MS);
    }
    shutdown_voltage_output();
    puts("Voltage monitoring thread stopped");
    return NULL;
}

void *app_climate_thread(void *argument)
{
    (void)argument;
    if (app_prepare_thread() < 0) {
        shutdown_climate_outputs();
        return NULL;
    }

    puts("Temperature and humidity monitoring thread started");
    while (keep_running) {
        update_climate_outputs();
        delay_ms(APP_POLL_INTERVAL_MS);
    }
    shutdown_climate_outputs();
    puts("Temperature and humidity monitoring thread stopped");
    return NULL;
}

void *app_proximity_thread(void *argument)
{
    (void)argument;
    if (app_prepare_thread() < 0) {
        shutdown_proximity_output();
        return NULL;
    }

    puts("Proximity monitoring thread started");
    while (keep_running) {
        update_proximity_buzzer();
        delay_ms(APP_POLL_INTERVAL_MS);
    }
    shutdown_proximity_output();
    puts("Proximity monitoring thread stopped");
    return NULL;
}
