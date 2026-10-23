#define _POSIX_C_SOURCE 200809L

#include "app.h"

#include "alarm.h"
#include "display.h"
#include "http_server.h"
#include "log.h"
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

#include <errno.h>
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
    app_state_t *st;

    /* 日志先行（读取 GRANARY_LOG_LEVEL / GRANARY_LOG_FILE） */
    log_init(NULL);

    if (state_init() < 0) {
        log_critical("state_init failed");
        return -1;
    }
    st = state_get();
    if (pthread_once(&signal_handlers_once, initialize_signal_handlers) != 0 ||
        !signal_handlers_ok) {
        log_critical("signal handler setup failed");
        return -1;
    }
    /* 快照目录（可配置），失败时由 snapshot_save_jpeg 回退 /tmp */
    snapshot_ensure_dir(st->cfg.snapshot_dir);
    snapshot_ensure_dir("/tmp/granary-snapshots");
#ifdef HAVE_LIBCURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    log_info("app initialized (port=%u, snapshot_dir=%s, keep_days=%d)",
             (unsigned)st->cfg.http_port, st->cfg.snapshot_dir,
             st->cfg.snapshot_keep_days);
    log_info("press KEY1 to start inspection (or set GRANARY_AUTO_START=1)");
    return 0;
}

void app_shutdown(void)
{
    alarm_shutdown_actuators();
#ifdef HAVE_LIBCURL
    curl_global_cleanup();
#endif
    state_shutdown();
    log_info("app shutdown complete");
    log_shutdown();
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
    unsigned int period_ms = st->cfg.sensor_period_ms;
    unsigned int display_ms = st->cfg.display_period_ms;

    (void)argument;
    log_info("sensor thread started (period=%ums)", period_ms);

    while (!st->stop) {
        float adc_mv = 0.0f;
        float temp;
        float hum;
        float lux;
        int pir;
        int flame;
        int valid = 1;
        int running;

        state_heartbeat(APP_THREAD_SENSOR);

        pthread_mutex_lock(&st->lock);
        running = st->inspection_running;
        pthread_mutex_unlock(&st->lock);

        if (!running) {
            delay_ms(period_ms);
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
        /* 读取失败时保留旧值，仅标记无效（显示/JSON 输出 ERR） */
        if (isfinite(temp) && temp != HT_SENSOR_ERROR_VALUE) {
            st->temperature_c = temp;
            st->temp_valid = 1;
        } else {
            st->temp_valid = 0;
        }
        if (isfinite(hum) && hum != HT_SENSOR_ERROR_VALUE) {
            st->humidity_rh = hum;
            st->hum_valid = 1;
        } else {
            st->hum_valid = 0;
        }
        if (isfinite(lux) && lux != LIGHT_SENSOR_ERROR_VALUE) {
            st->illuminance = lux;
            st->lux_valid = 1;
        } else {
            st->lux_valid = 0;
        }
        /* Missing PIR/flame sysfs: treat as inactive, not error */
        st->pir_detected = (pir == 1);
        st->flame_detected = (flame == 1);

        st->current_a = compute_current_a(st);
        st->power_w = st->supply_voltage_v * st->current_a;

        alarm_evaluate(st);

        if (st->e01 && !prev_e01) {
            st->snapshot_request = 1;
            snprintf(st->snapshot_tag, sizeof(st->snapshot_tag), "alarm");
        }
        if ((st->e03 || st->e04 || st->e05) && !prev_severe) {
            st->snapshot_request = 1;
            snprintf(st->snapshot_tag, sizeof(st->snapshot_tag), "alarm");
        }
        prev_e01 = st->e01;
        prev_severe = st->e03 || st->e04 || st->e05;
        pthread_mutex_unlock(&st->lock);

        display_acc += period_ms;
        if (display_acc >= display_ms) {
            pthread_mutex_lock(&st->lock);
            display_refresh(st);
            display_framebuffer_stub(st);
            pthread_mutex_unlock(&st->lock);
            display_acc = 0;
        }

        delay_ms(period_ms);
    }

    log_info("sensor thread stopped");
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
        log_warn("key device unavailable; KEY1/2/3 disabled "
                 "(force inspection with GRANARY_AUTO_START=1)");
        if (getenv("GRANARY_AUTO_START") != NULL) {
            pthread_mutex_lock(&st->lock);
            st->inspection_running = 1;
            pthread_mutex_unlock(&st->lock);
            log_info("GRANARY_AUTO_START: inspection running");
        }
        while (!st->stop) {
            state_heartbeat(APP_THREAD_KEY);
            delay_ms(200);
        }
        return NULL;
    }

    log_info("key thread on %s (KEY1=start/pause KEY2=snap+AI KEY3=mute)",
             keys.device_path);

    while (!st->stop) {
        int rc = key_read_event(&keys, &event);
        if (rc < 0) {
            delay_ms(50);
            continue;
        }
        if (rc == 0) {
            state_heartbeat(APP_THREAD_KEY);
            delay_ms(20);
            continue;
        }
        if (event.type != KEY_EVENT_PRESS) {
            continue;
        }

        if (event.code == GRANARY_KEY1_CODE) {
            pthread_mutex_lock(&st->lock);
            st->inspection_running = !st->inspection_running;
            log_info("KEY1: inspection %s",
                     st->inspection_running ? "STARTED" : "PAUSED");
            if (!st->inspection_running) {
                snprintf(st->ai_text, sizeof(st->ai_text),
                         "Inspection paused");
            }
            pthread_mutex_unlock(&st->lock);
        } else if (event.code == GRANARY_KEY2_CODE) {
            /*
             * KEY2：手动抓拍并即时提交多模态分析（PDF 4.4）。
             * 默认不落盘；GRANARY_KEY2_SAVE=1 时额外保存 JPG（tag=manual）。
             */
            log_info("KEY2: manual snapshot + multimodal request");
            if (st->cfg.key2_save_snapshot) {
                state_request_snapshot("manual");
            }
            state_request_ai();
        } else if (event.code == GRANARY_KEY3_CODE) {
            pthread_mutex_lock(&st->lock);
            st->buzzer_muted = !st->buzzer_muted;
            log_info("KEY3: buzzer mute %s", st->buzzer_muted ? "ON" : "OFF");
            pthread_mutex_unlock(&st->lock);
        }
    }

    key_close(&keys);
    log_info("key thread stopped");
    return NULL;
}

void *app_camera_thread(void *argument)
{
    app_state_t *st = state_get();
    camera_t cam;
    int cam_ok = 0;
    unsigned int ai_acc = 0;
    unsigned int camera_ms = st->cfg.camera_period_ms;
    unsigned int ai_ms = st->cfg.ai_period_ms;

    (void)argument;
    memset(&cam, 0, sizeof(cam));

    /* NULL: CAMERA_DEVICE env, else probe /dev/video1 → video2 → video0 */
    if (camera_open(&cam, NULL) == 0 && camera_start(&cam) == 0) {
        cam_ok = 1;
        log_info("camera opened on %s (%ux%u)", cam.device_path, cam.width,
                 cam.height);
    } else {
        log_warn("camera unavailable; will retry every %ums",
                 (unsigned)CAMERA_RETRY_MS);
    }

    log_info("camera thread started (period=%ums, ai=%ums)", camera_ms, ai_ms);
    while (!st->stop) {
        int running;
        int want_snap;
        int want_ai_periodic = 0;
        unsigned char *jpeg = NULL;
        size_t jpeg_len = 0;
        char tag[32];

        state_heartbeat(APP_THREAD_CAMERA);

        /* 摄像头未打开：周期性重连（支持热插拔恢复） */
        if (!cam_ok) {
            if (camera_open(&cam, NULL) == 0 && camera_start(&cam) == 0) {
                cam_ok = 1;
                log_info("camera reconnected on %s", cam.device_path);
            } else {
                camera_close(&cam);
                memset(&cam, 0, sizeof(cam));
                delay_ms(CAMERA_RETRY_MS);
                continue;
            }
        }

        pthread_mutex_lock(&st->lock);
        running = st->inspection_running;
        want_snap = st->snapshot_request;
        snprintf(tag, sizeof(tag), "%s", st->snapshot_tag);
        pthread_mutex_unlock(&st->lock);

        if (!running) {
            delay_ms(200);
            continue;
        }

        if (camera_capture_jpeg(&cam, &jpeg, &jpeg_len) == 0) {
            state_set_jpeg(jpeg, jpeg_len);

            pthread_mutex_lock(&st->lock);
            want_snap = st->snapshot_request;
            snprintf(tag, sizeof(tag), "%s", st->snapshot_tag);
            if (want_snap) {
                st->snapshot_request = 0;
            }
            pthread_mutex_unlock(&st->lock);

            if (want_snap) {
                const char *dir;
                int saved;
                pthread_mutex_lock(&st->lock);
                dir = st->snapshot_dir;
                pthread_mutex_unlock(&st->lock);
                saved = snapshot_save_jpeg(dir, jpeg, jpeg_len, tag);
                if (saved < 0) {
                    saved = snapshot_save_jpeg("/tmp/granary-snapshots", jpeg,
                                               jpeg_len, tag);
                }
                if (saved == 0) {
                    pthread_mutex_lock(&st->lock);
                    st->snapshot_count++;
                    pthread_mutex_unlock(&st->lock);
                }
            }

            free(jpeg);
        } else {
            /*
             * 采集失败（摄像头中途掉线 / USB 复位等）：必须关闭失效 fd 并
             * 清除 cam_ok，回到上面的重连分支。否则会拿着死掉的设备描述符
             * 每周期重试，错误刷屏且 AI 永远拿不到帧。
             */
            log_warn("camera capture failed on %s (%s); reconnecting",
                     cam.device_path, strerror(errno));
            camera_close(&cam);
            memset(&cam, 0, sizeof(cam));
            cam_ok = 0;
            delay_ms(CAMERA_RETRY_MS);
            continue;
        }

        ai_acc += camera_ms;
        if (ai_acc >= ai_ms) {
            want_ai_periodic = 1;
            ai_acc = 0;
        }
        if (want_ai_periodic) {
            state_request_ai();
        }

        delay_ms(camera_ms);
    }

    if (cam_ok) {
        camera_close(&cam);
    }
    log_info("camera thread stopped");
    return NULL;
}

void *app_alarm_thread(void *argument)
{
    app_state_t *st = state_get();
    unsigned int tick = 0;
    unsigned int period_ms = st->cfg.alarm_period_ms;

    (void)argument;
    log_info("alarm actuator thread started (period=%ums)", period_ms);
    while (!st->stop) {
        state_heartbeat(APP_THREAD_ALARM);

        pthread_mutex_lock(&st->lock);
        /* 执行机构状态由 alarm_apply_actuators 直接写 sysfs 并回写 shadow */
        alarm_apply_actuators(st, tick);
        pthread_mutex_unlock(&st->lock);

        tick += period_ms;
        delay_ms(period_ms);
    }
    alarm_shutdown_actuators();
    log_info("alarm actuator thread stopped");
    return NULL;
}

void *app_ai_thread(void *argument)
{
    app_state_t *st = state_get();

    (void)argument;
    log_info("AI thread started");
    while (!st->stop) {
        int do_ai = 0;
        unsigned char *jpeg = NULL;
        size_t jpeg_len = 0;

        state_heartbeat(APP_THREAD_AI);

        pthread_mutex_lock(&st->lock);
        while (!st->stop && !st->ai_request) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&st->ai_cond, &st->lock, &ts);
            if (!st->stop && !st->ai_request) {
                /*
                 * 空闲等待期间也要喂看门狗：无摄像头/无按键的开发机上
                 * 可能长时间没有 AI 请求，不更新心跳会被看门狗误判卡死。
                 */
                pthread_mutex_unlock(&st->lock);
                state_heartbeat(APP_THREAD_AI);
                pthread_mutex_lock(&st->lock);
            }
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

        /*
         * 关键修复：multimodal_analyze_jpeg 内部是阻塞式 HTTPS 请求
         * （curl 超时最长 60s），绝不能在持有 st->lock 时调用。
         * 之前在锁内调用，导致 AI 请求期间 /stream 推流、/status.json
         * 轮询、HTTP accept（state_heartbeat 也要拿同一把锁）全部
         * 阻塞——网页表现为"有视频推流时卡死"。而周期性 AI 请求只有
         * 摄像头采集成功后才会触发，所以没摄像头时一切正常。
         * 现在网络请求阶段不加锁，结果由 multimodal 内部短暂加锁提交。
         */
        multimodal_analyze_jpeg(st, jpeg, jpeg_len);

        pthread_mutex_lock(&st->lock);
        alarm_evaluate(st);
        pthread_mutex_unlock(&st->lock);
        free(jpeg);
    }
    log_info("AI thread stopped");
    return NULL;
}

void *app_watchdog_thread(void *argument)
{
    app_state_t *st = state_get();

    (void)argument;
    log_info("watchdog started (stall threshold %ds)", WATCHDOG_STALL_S);
    while (!st->stop) {
        time_t now = time(NULL);
        int i;

        state_heartbeat(APP_THREAD_WATCHDOG);
        delay_ms(WATCHDOG_PERIOD_MS);

        for (i = 0; i < APP_THREAD_COUNT; i++) {
            time_t hb;

            pthread_mutex_lock(&st->lock);
            hb = st->heartbeat[i];
            pthread_mutex_unlock(&st->lock);

            if (hb == 0) {
                continue; /* 线程尚未开始运行 */
            }
            if (now - hb > WATCHDOG_STALL_S) {
                log_critical("watchdog: thread slot %d stalled %lds; "
                             "stopping app (systemd will restart)",
                             i, (long)(now - hb));
                app_request_stop();
                return NULL;
            }
        }
    }
    log_info("watchdog stopped");
    return NULL;
}
