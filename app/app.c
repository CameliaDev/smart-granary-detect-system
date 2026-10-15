#define _POSIX_C_SOURCE 200809L

#include "app.h"

#include "alarm.h"
#include "display.h"
#include "http_server.h"
#include "multimodal.h"
#include "snapshot.h"
#include "state.h"

#include "camera.h"
#include "delay.h"
#include "flame_sensor.h"
#include "ht_sensor.h"
#include "key.h"
#include "light_sensor.h"
#include "pir_sensor.h"
#include "voltage_sensor.h"

#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#ifndef CAMERA_PERIOD_MS
#define CAMERA_PERIOD_MS 2000U
#endif
#ifndef SENSOR_PERIOD_MS
#define SENSOR_PERIOD_MS 200U
#endif
#ifndef AI_PERIOD_MS
#define AI_PERIOD_MS 15000U
#endif
#ifndef DISPLAY_PERIOD_MS
#define DISPLAY_PERIOD_MS 2000U
#endif

#ifndef GRANARY_KEY1_CODE
#define GRANARY_KEY1_CODE KEY_1
#endif
#ifndef GRANARY_KEY2_CODE
#define GRANARY_KEY2_CODE KEY_2
#endif
#ifndef GRANARY_KEY3_CODE
#define GRANARY_KEY3_CODE KEY_3
#endif

static pthread_once_t signal_handlers_once = PTHREAD_ONCE_INIT;
static int signal_handlers_ok;

static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    app_request_stop();
}

static void initialize_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        signal_handlers_ok = 0;
        return;
    }
    signal_handlers_ok = 1;
}

void app_request_stop(void)
{
    app_state_t *st = state_get();

    st->stop = 1;
    pthread_mutex_lock(&st->lock);
    pthread_cond_broadcast(&st->ai_cond);
    pthread_cond_broadcast(&st->frame_cond);
    pthread_mutex_unlock(&st->lock);
}

int app_init(void)
{
    if (state_init() < 0) {
        return -1;
    }
    if (pthread_once(&signal_handlers_once, initialize_signal_handlers) != 0 ||
        !signal_handlers_ok) {
        return -1;
    }
    snapshot_ensure_dir("./snapshots");
    snapshot_ensure_dir("/tmp/granary-snapshots");
#ifdef HAVE_LIBCURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    puts("Granary app initialized. Press KEY1 to start inspection.");
    return 0;
}

void app_shutdown(void)
{
    alarm_shutdown_actuators();
#ifdef HAVE_LIBCURL
    curl_global_cleanup();
#endif
    state_shutdown();
}

static float compute_current_a(const app_state_t *st)
{
    float i = BASE_CURRENT_A;

    if (st->fan_on) {
        i += FAN_CURRENT_A;
    }
    if (st->buzzer_sounding) {
        i += BUZZER_CURRENT_A;
    }
    if (st->leds_all_blinking) {
        i += LED_BLINK_CURRENT_A;
    }
    return i;
}

void *app_sensor_thread(void *argument)
{
    app_state_t *st = state_get();
    int prev_e01 = 0;
    int prev_severe = 0;
    unsigned int display_acc = 0;

    (void)argument;
    puts("Sensor thread started");

    while (!st->stop) {
        float adc_mv = 0.0f;
        float temp;
        float hum;
        float lux;
        int pir;
        int flame;
        int valid = 1;
        int running;

        pthread_mutex_lock(&st->lock);
        running = st->inspection_running;
        pthread_mutex_unlock(&st->lock);

        if (!running) {
            delay_ms(SENSOR_PERIOD_MS);
            continue;
        }

        if (voltage_sensor_read(&adc_mv) < 0 || !isfinite(adc_mv)) {
            valid = 0;
            adc_mv = 0.0f;
        }
        temp = ht_read_temperature();
        hum = ht_read_humidity();
        lux = light_read();
        pir = pir_sensor_read();
        flame = flame_sensor_read();

        pthread_mutex_lock(&st->lock);
        st->sensors_valid = valid;
        st->adc_voltage_v = adc_mv / 1000.0f;
        st->supply_voltage_v = st->adc_voltage_v * 2.0f;
        if (isfinite(temp) && temp != HT_SENSOR_ERROR_VALUE) {
            st->temperature_c = temp;
        }
        if (isfinite(hum) && hum != HT_SENSOR_ERROR_VALUE) {
            st->humidity_rh = hum;
        }
        if (isfinite(lux) && lux != LIGHT_SENSOR_ERROR_VALUE) {
            st->illuminance = lux;
        }
        /* Missing PIR/flame sysfs: treat as inactive, not error */
        st->pir_detected = (pir == 1);
        st->flame_detected = (flame == 1);

        st->current_a = compute_current_a(st);
        st->power_w = st->supply_voltage_v * st->current_a;

        alarm_evaluate(st);

        if (st->e01 && !prev_e01) {
            st->snapshot_request = 1;
        }
        if ((st->e03 || st->e04 || st->e05) && !prev_severe) {
            st->snapshot_request = 1;
        }
        prev_e01 = st->e01;
        prev_severe = st->e03 || st->e04 || st->e05;
        pthread_mutex_unlock(&st->lock);

        display_acc += SENSOR_PERIOD_MS;
        if (display_acc >= DISPLAY_PERIOD_MS) {
            pthread_mutex_lock(&st->lock);
            display_refresh(st);
            display_framebuffer_stub(st);
            pthread_mutex_unlock(&st->lock);
            display_acc = 0;
        }

        delay_ms(SENSOR_PERIOD_MS);
    }

    puts("Sensor thread stopped");
    return NULL;
}

void *app_key_thread(void *argument)
{
    app_state_t *st = state_get();
    key_device_t keys;
    key_event_t event;

    (void)argument;
    memset(&keys, 0, sizeof(keys));

    if (key_init_auto(&keys) < 0) {
        fprintf(stderr,
                "Key device unavailable; KEY1/2/3 disabled "
                "(inspection can be forced with GRANARY_AUTO_START=1)\n");
        if (getenv("GRANARY_AUTO_START") != NULL) {
            pthread_mutex_lock(&st->lock);
            st->inspection_running = 1;
            pthread_mutex_unlock(&st->lock);
            puts("GRANARY_AUTO_START: inspection running");
        }
        while (!st->stop) {
            delay_ms(200);
        }
        return NULL;
    }

    printf("Key thread on %s (KEY1=start/pause KEY2=snap+AI KEY3=mute)\n",
           keys.device_path);

    while (!st->stop) {
        int rc = key_read_event(&keys, &event);
        if (rc < 0) {
            delay_ms(50);
            continue;
        }
        if (rc == 0) {
            delay_ms(20);
            continue;
        }
        if (event.type != KEY_EVENT_PRESS) {
            continue;
        }

        if (event.code == GRANARY_KEY1_CODE) {
            pthread_mutex_lock(&st->lock);
            st->inspection_running = !st->inspection_running;
            printf("KEY1: inspection %s\n",
                   st->inspection_running ? "STARTED" : "PAUSED");
            if (!st->inspection_running) {
                snprintf(st->ai_text, sizeof(st->ai_text),
                         "Inspection paused");
            }
            pthread_mutex_unlock(&st->lock);
        } else if (event.code == GRANARY_KEY2_CODE) {
            puts("KEY2: manual snapshot + multimodal request");
            state_request_snapshot();
            state_request_ai();
        } else if (event.code == GRANARY_KEY3_CODE) {
            pthread_mutex_lock(&st->lock);
            st->buzzer_muted = !st->buzzer_muted;
            printf("KEY3: buzzer mute %s\n",
                   st->buzzer_muted ? "ON" : "OFF");
            pthread_mutex_unlock(&st->lock);
        }
    }

    key_close(&keys);
    puts("Key thread stopped");
    return NULL;
}

void *app_camera_thread(void *argument)
{
    app_state_t *st = state_get();
    camera_t cam;
    int cam_ok = 0;
    unsigned int ai_acc = 0;

    (void)argument;
    memset(&cam, 0, sizeof(cam));

    /* NULL: CAMERA_DEVICE env, else probe /dev/video1 → video2 → video0 */
    if (camera_open(&cam, NULL) == 0 && camera_start(&cam) == 0) {
        cam_ok = 1;
    } else {
        fprintf(stderr,
                "Camera unavailable; stream/AI will wait for frames\n");
    }

    puts("Camera thread started");
    while (!st->stop) {
        int running;
        int want_snap;
        int want_ai_periodic = 0;
        unsigned char *jpeg = NULL;
        size_t jpeg_len = 0;

        pthread_mutex_lock(&st->lock);
        running = st->inspection_running;
        want_snap = st->snapshot_request;
        pthread_mutex_unlock(&st->lock);

        if (!running || !cam_ok) {
            delay_ms(200);
            continue;
        }

        if (camera_capture_jpeg(&cam, &jpeg, &jpeg_len) == 0) {
            state_set_jpeg(jpeg, jpeg_len);

            pthread_mutex_lock(&st->lock);
            want_snap = st->snapshot_request;
            if (want_snap) {
                st->snapshot_request = 0;
            }
            pthread_mutex_unlock(&st->lock);

            if (want_snap) {
                const char *dir;
                pthread_mutex_lock(&st->lock);
                dir = st->snapshot_dir;
                pthread_mutex_unlock(&st->lock);
                if (snapshot_save_jpeg(dir, jpeg, jpeg_len, "alarm") < 0) {
                    snapshot_save_jpeg("/tmp/granary-snapshots", jpeg,
                                       jpeg_len, "alarm");
                }
            }

            free(jpeg);
        }

        ai_acc += CAMERA_PERIOD_MS;
        if (ai_acc >= AI_PERIOD_MS) {
            want_ai_periodic = 1;
            ai_acc = 0;
        }
        if (want_ai_periodic) {
            state_request_ai();
        }

        delay_ms(CAMERA_PERIOD_MS);
    }

    if (cam_ok) {
        camera_close(&cam);
    }
    puts("Camera thread stopped");
    return NULL;
}

void *app_alarm_thread(void *argument)
{
    app_state_t *st = state_get();
    unsigned int tick = 0;

    (void)argument;
    puts("Alarm actuator thread started");
    while (!st->stop) {
        int fan_on;
        int buzzer_sounding;
        int leds_all_blinking;

        pthread_mutex_lock(&st->lock);
        alarm_apply_actuators(st, tick);
        fan_on = st->fan_on;
        buzzer_sounding = st->buzzer_sounding;
        leds_all_blinking = st->leds_all_blinking;
        (void)fan_on;
        (void)buzzer_sounding;
        (void)leds_all_blinking;
        pthread_mutex_unlock(&st->lock);

        tick += 50;
        delay_ms(50);
    }
    alarm_shutdown_actuators();
    puts("Alarm actuator thread stopped");
    return NULL;
}

void *app_ai_thread(void *argument)
{
    app_state_t *st = state_get();

    (void)argument;
    puts("AI thread started");
    while (!st->stop) {
        int do_ai = 0;
        unsigned char *jpeg = NULL;
        size_t jpeg_len = 0;

        pthread_mutex_lock(&st->lock);
        while (!st->stop && !st->ai_request) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&st->ai_cond, &st->lock, &ts);
        }
        if (st->stop) {
            pthread_mutex_unlock(&st->lock);
            break;
        }
        if (st->ai_request && st->inspection_running) {
            st->ai_request = 0;
            do_ai = 1;
        } else {
            st->ai_request = 0;
        }
        pthread_mutex_unlock(&st->lock);

        if (!do_ai) {
            continue;
        }
        if (state_copy_jpeg(&jpeg, &jpeg_len, NULL) < 0) {
            pthread_mutex_lock(&st->lock);
            snprintf(st->ai_text, sizeof(st->ai_text),
                     "No camera frame for AI yet");
            pthread_mutex_unlock(&st->lock);
            continue;
        }

        pthread_mutex_lock(&st->lock);
        multimodal_analyze_jpeg(st, jpeg, jpeg_len);
        alarm_evaluate(st);
        pthread_mutex_unlock(&st->lock);
        free(jpeg);
    }
    puts("AI thread stopped");
    return NULL;
}
