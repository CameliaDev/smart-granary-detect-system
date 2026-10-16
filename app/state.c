/*
 * state.c —— 全局状态管理
 *
 * 除原有的状态存取外，本模块还负责：
 *   - 运行期配置加载：全部阈值/周期/端口/路径均可用环境变量覆盖
 *     （默认值取各模块宏，见 alarm.h / app.c / http_server.h）；
 *   - 线程心跳维护（看门狗线程据此判断各线程是否卡死）；
 *   - 告警历史环形缓冲记录。
 */

#include "state.h"

#include "alarm.h"
#include "app.h"
#include "http_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_state_t g_state;

app_state_t *state_get(void)
{
    return &g_state;
}

/* ---------- 环境变量解析辅助 ---------- */

static unsigned int env_u32(const char *name, unsigned int def)
{
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long v;

    if (s == NULL || s[0] == '\0') {
        return def;
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    if (end == s || (end != NULL && *end != '\0') || errno == ERANGE ||
        v > 3600000UL) {
        return def;
    }
    return (unsigned int)v;
}

static int env_int(const char *name, int def, int min, int max)
{
    const char *s = getenv(name);
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0') {
        return def;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || (end != NULL && *end != '\0') || errno == ERANGE ||
        v < min || v > max) {
        return def;
    }
    return (int)v;
}

static float env_float(const char *name, float def, float min, float max)
{
    const char *s = getenv(name);
    char *end = NULL;
    float v;

    if (s == NULL || s[0] == '\0') {
        return def;
    }
    errno = 0;
    v = strtof(s, &end);
    if (end == s || (end != NULL && *end != '\0') || errno == ERANGE ||
        v < min || v > max) {
        return def;
    }
    return v;
}

/* ---------- 配置加载 ---------- */

static void config_load(granary_config_t *cfg)
{
    /* 告警阈值（默认值与 alarm.h 宏保持一致） */
    cfg->voltage_min_v = env_float("GRANARY_VOLTAGE_MIN",
                                   VOLTAGE_SAFE_MIN_V, 0.0f, 20.0f);
    cfg->voltage_max_v = env_float("GRANARY_VOLTAGE_MAX",
                                   VOLTAGE_SAFE_MAX_V, 0.0f, 20.0f);
    cfg->temp_max_c    = env_float("GRANARY_TEMP_MAX",
                                   TEMP_SAFE_MAX_C, -40.0f, 200.0f);
    cfg->rh_max_rh     = env_float("GRANARY_RH_MAX",
                                   HUMIDITY_SAFE_MAX_RH, 0.0f, 100.0f);

    /* 执行机构参数 */
    cfg->fan_alarm_pwm  = env_u32("GRANARY_FAN_PWM", FAN_ALARM_PWM);
    if (cfg->fan_alarm_pwm > 255U) {
        cfg->fan_alarm_pwm = 255U;
    }
    cfg->buzzer_low_hz  = env_u32("GRANARY_BUZZER_LOW_HZ", BUZZER_LOW_FREQ_HZ);
    cfg->buzzer_high_hz = env_u32("GRANARY_BUZZER_HIGH_HZ", BUZZER_HIGH_FREQ_HZ);

    /* 任务周期 */
    cfg->sensor_period_ms  = env_u32("GRANARY_SENSOR_PERIOD_MS",
                                     SENSOR_PERIOD_MS);
    cfg->camera_period_ms  = env_u32("GRANARY_CAMERA_PERIOD_MS",
                                     CAMERA_PERIOD_MS);
    cfg->ai_period_ms      = env_u32("GRANARY_AI_PERIOD_MS", AI_PERIOD_MS);
    cfg->display_period_ms = env_u32("GRANARY_DISPLAY_PERIOD_MS",
                                     DISPLAY_PERIOD_MS);
    cfg->alarm_period_ms   = env_u32("GRANARY_ALARM_PERIOD_MS",
                                     ALARM_PERIOD_MS);
    if (cfg->sensor_period_ms < 20U)  cfg->sensor_period_ms = 20U;
    if (cfg->camera_period_ms < 50U)  cfg->camera_period_ms = 50U;
    if (cfg->alarm_period_ms < 10U)   cfg->alarm_period_ms = 10U;

    /* 网络与存储 */
    cfg->http_port = (unsigned short)env_int("GRANARY_HTTP_PORT",
                                             HTTP_SERVER_PORT, 1, 65535);
    cfg->snapshot_keep_days = env_int("GRANARY_SNAPSHOT_KEEP_DAYS", 7, 0, 3650);
    cfg->key2_save_snapshot = env_int("GRANARY_KEY2_SAVE", 0, 0, 1);
    {
        const char *dir = getenv("GRANARY_SNAPSHOT_DIR");
        if (dir != NULL && dir[0] != '\0') {
            snprintf(cfg->snapshot_dir, sizeof(cfg->snapshot_dir), "%s", dir);
        } else {
            snprintf(cfg->snapshot_dir, sizeof(cfg->snapshot_dir), "%s",
                     "./snapshots");
        }
    }
    /*
     * 多模态（火山方舟 Responses API）：
     * 默认 BASE 与模型名均为方舟 doubao 系列，可用环境变量覆盖。
     */
    {
        const char *base = getenv("MULTIMODAL_API_BASE");
        if (base != NULL && base[0] != '\0') {
            snprintf(cfg->multimodal_api_base,
                     sizeof(cfg->multimodal_api_base), "%s", base);
        } else {
            snprintf(cfg->multimodal_api_base,
                     sizeof(cfg->multimodal_api_base),
                     "https://ark.cn-beijing.volces.com/api/v3");
        }
    }
    {
        const char *model = getenv("MULTIMODAL_MODEL");
        if (model != NULL && model[0] != '\0') {
            snprintf(cfg->multimodal_model, sizeof(cfg->multimodal_model),
                     "%s", model);
        } else {
            snprintf(cfg->multimodal_model, sizeof(cfg->multimodal_model),
                     "doubao-seed-2-1-lite-260915");
        }
    }
    /* 多模态提示词：留空表示使用 multimodal.c 内置默认提示词 */
    {
        const char *prompt = getenv("MULTIMODAL_PROMPT");
        if (prompt != NULL && prompt[0] != '\0') {
            snprintf(cfg->multimodal_prompt, sizeof(cfg->multimodal_prompt),
                     "%s", prompt);
        } else {
            cfg->multimodal_prompt[0] = '\0';
        }
    }
}

/* ---------- 状态接口 ---------- */

int state_init(void)
{
    memset(&g_state, 0, sizeof(g_state));
    if (pthread_mutex_init(&g_state.lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&g_state.frame_cond, NULL) != 0) {
        pthread_mutex_destroy(&g_state.lock);
        return -1;
    }
    if (pthread_cond_init(&g_state.ai_cond, NULL) != 0) {
        pthread_cond_destroy(&g_state.frame_cond);
        pthread_mutex_destroy(&g_state.lock);
        return -1;
    }
    g_state.inspection_running = 0;
    g_state.buzzer_muted = 0;
    snprintf(g_state.snapshot_dir, sizeof(g_state.snapshot_dir), "./snapshots");
    snprintf(g_state.ai_text, sizeof(g_state.ai_text),
             "Waiting for inspection start (KEY1)");

    /* 运行期配置（环境变量覆盖）与统计/看门狗初始化 */
    config_load(&g_state.cfg);
    g_state.start_time = time(NULL);
    g_state.snapshot_count = 0;

    return 0;
}

void state_shutdown(void)
{
    pthread_mutex_lock(&g_state.lock);
    free(g_state.jpeg);
    g_state.jpeg = NULL;
    g_state.jpeg_len = 0;
    g_state.jpeg_cap = 0;
    pthread_mutex_unlock(&g_state.lock);
    pthread_cond_destroy(&g_state.ai_cond);
    pthread_cond_destroy(&g_state.frame_cond);
    pthread_mutex_destroy(&g_state.lock);
}

void state_set_jpeg(const unsigned char *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    pthread_mutex_lock(&g_state.lock);
    if (g_state.jpeg_cap < len) {
        unsigned char *n = realloc(g_state.jpeg, len);
        if (n == NULL) {
            pthread_mutex_unlock(&g_state.lock);
            return;
        }
        g_state.jpeg = n;
        g_state.jpeg_cap = len;
    }
    memcpy(g_state.jpeg, data, len);
    g_state.jpeg_len = len;
    g_state.jpeg_seq++;
    pthread_cond_broadcast(&g_state.frame_cond);
    pthread_mutex_unlock(&g_state.lock);
}

int state_copy_jpeg(unsigned char **out, size_t *out_len, uint64_t *seq)
{
    unsigned char *copy;

    if (out == NULL || out_len == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_state.lock);
    if (g_state.jpeg == NULL || g_state.jpeg_len == 0) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }
    copy = malloc(g_state.jpeg_len);
    if (copy == NULL) {
        pthread_mutex_unlock(&g_state.lock);
        return -1;
    }
    memcpy(copy, g_state.jpeg, g_state.jpeg_len);
    *out = copy;
    *out_len = g_state.jpeg_len;
    if (seq != NULL) {
        *seq = g_state.jpeg_seq;
    }
    pthread_mutex_unlock(&g_state.lock);
    return 0;
}

void state_request_ai(void)
{
    pthread_mutex_lock(&g_state.lock);
    g_state.ai_request = 1;
    pthread_cond_signal(&g_state.ai_cond);
    pthread_mutex_unlock(&g_state.lock);
}

void state_request_snapshot(const char *tag)
{
    pthread_mutex_lock(&g_state.lock);
    g_state.snapshot_request = 1;
    snprintf(g_state.snapshot_tag, sizeof(g_state.snapshot_tag), "%s",
             (tag != NULL && tag[0] != '\0') ? tag : "alarm");
    pthread_mutex_unlock(&g_state.lock);
}

void state_heartbeat(int slot)
{
    app_state_t *st = &g_state;

    if (slot < 0 || slot >= STATE_THREAD_SLOTS) {
        return;
    }
    pthread_mutex_lock(&st->lock);
    st->heartbeat[slot] = time(NULL);
    pthread_mutex_unlock(&st->lock);
}

void state_push_alarm_history_locked(const char *text)
{
    app_state_t *st = &g_state;
    int idx;

    if (text == NULL) {
        return;
    }
    idx = st->alarm_history_head;
    snprintf(st->alarm_history_text[idx], STATE_ALARM_TEXT_MAX, "%s", text);
    st->alarm_history_time[idx] = time(NULL);
    st->alarm_history_head = (idx + 1) % STATE_ALARM_HISTORY_MAX;
    if (st->alarm_history_count < STATE_ALARM_HISTORY_MAX) {
        st->alarm_history_count++;
    }
}
