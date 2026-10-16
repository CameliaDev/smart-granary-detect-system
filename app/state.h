#ifndef STATE_H
#define STATE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define STATE_AI_TEXT_MAX 1024
#define STATE_ALARM_TEXT_MAX 256
#define STATE_OVERLAY_MAX 512

/* 告警历史环形缓冲容量 */
#define STATE_ALARM_HISTORY_MAX 16
/* 线程看门狗心跳槽位数（须 >= app.h 的 APP_THREAD_COUNT） */
#define STATE_THREAD_SLOTS 8

/*
 * 运行期配置：默认值来自各模块宏（见 alarm.h / app.c / http_server.h），
 * 均可由环境变量覆盖，无需重新编译。详见 README「环境变量」一节。
 */
typedef struct {
    /* 告警阈值 */
    float voltage_min_v;          /* GRANARY_VOLTAGE_MIN  (默认 4.8)  */
    float voltage_max_v;          /* GRANARY_VOLTAGE_MAX  (默认 5.2)  */
    float temp_max_c;             /* GRANARY_TEMP_MAX     (默认 30)   */
    float rh_max_rh;              /* GRANARY_RH_MAX       (默认 65)   */
    /* 执行机构参数 */
    unsigned int fan_alarm_pwm;   /* GRANARY_FAN_PWM      (默认 200)  */
    unsigned int buzzer_low_hz;   /* GRANARY_BUZZER_LOW_HZ  (默认 800)  */
    unsigned int buzzer_high_hz;  /* GRANARY_BUZZER_HIGH_HZ (默认 2500) */
    /* 任务周期 (ms) */
    unsigned int sensor_period_ms;  /* GRANARY_SENSOR_PERIOD_MS  (默认 200)  */
    unsigned int camera_period_ms;  /* GRANARY_CAMERA_PERIOD_MS  (默认 200)  */
    unsigned int ai_period_ms;      /* GRANARY_AI_PERIOD_MS      (默认 15000) */
    unsigned int display_period_ms; /* GRANARY_DISPLAY_PERIOD_MS (默认 2000)  */
    unsigned int alarm_period_ms;   /* GRANARY_ALARM_PERIOD_MS   (默认 50)    */
    /* 网络与存储 */
    unsigned short http_port;       /* GRANARY_HTTP_PORT (默认 8081) */
    char snapshot_dir[256];         /* GRANARY_SNAPSHOT_DIR (默认 ./snapshots) */
    int snapshot_keep_days;         /* GRANARY_SNAPSHOT_KEEP_DAYS (默认 7, 0=不清理) */
    int key2_save_snapshot;         /* GRANARY_KEY2_SAVE (默认 0; PDF 定义 KEY2 仅抓拍分析) */
    /* 多模态（火山方舟 Responses API） */
    char multimodal_api_base[256]; /* MULTIMODAL_API_BASE，默认 https://ark.cn-beijing.volces.com/api/v3 */
    char multimodal_model[64];     /* MULTIMODAL_MODEL，默认 doubao-seed-2-1-lite-260915 */
    char multimodal_prompt[512];    /* MULTIMODAL_PROMPT (默认内置巡检提示词) */
} granary_config_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t frame_cond;
    pthread_cond_t ai_cond;

    volatile int stop;
    int inspection_running; /* KEY1: 0=paused, 1=running */
    int buzzer_muted;       /* KEY3 */

    /* Sensors / power */
    float adc_voltage_v;    /* sensor 0~3 V */
    float supply_voltage_v; /* U = adc * 2 */
    float current_a;
    float power_w;
    float temperature_c;
    float humidity_rh;
    float illuminance;
    int pir_detected;
    int flame_detected;
    int sensors_valid;
    /* 分项有效性：读取失败时保留旧值但标记无效，显示/JSON 输出 ERR */
    int temp_valid;
    int hum_valid;
    int lux_valid;

    /* Actuator shadow for current model */
    int fan_on;
    int buzzer_sounding;
    int leds_all_blinking;

    /* Alarms E01..E05 */
    int e01;
    int e02;
    int e03;
    int e04;
    int e05;
    char alarm_text[STATE_ALARM_TEXT_MAX];
    char overlay_text[STATE_OVERLAY_MAX];

    /*
     * 告警历史（环形缓冲）：
     * alarm_text 每次变化记录一条，head 指向下一条写入位置，
     * count 为已记录条数（上限 STATE_ALARM_HISTORY_MAX）。
     * 最新一条索引为 (head - 1 + MAX) % MAX。
     */
    int alarm_history_count;
    int alarm_history_head;
    time_t alarm_history_time[STATE_ALARM_HISTORY_MAX];
    char alarm_history_text[STATE_ALARM_HISTORY_MAX][STATE_ALARM_TEXT_MAX];

    /* AI */
    char ai_text[STATE_AI_TEXT_MAX];
    int ai_intrusion;
    int ai_mold;
    int ai_collapse;
    int ai_pest;        /* 虫害堆积（E04） */
    int ai_debris;      /* 杂物遗留（E04） */
    int ai_fire;
    int ai_request; /* set by KEY2 or periodic */
    uint64_t ai_seq;

    /* Latest JPEG for stream / snapshot */
    unsigned char *jpeg;
    size_t jpeg_len;
    size_t jpeg_cap;
    uint64_t jpeg_seq;
    int snapshot_request;
    char snapshot_tag[32];       /* 快照标签：alarm=告警自动抓拍, manual=按键手动 */

    /* Snapshot dir */
    char snapshot_dir[256];

    /* 运行期配置（环境变量覆盖，state_init 时加载） */
    granary_config_t cfg;

    /* 统计与看门狗 */
    uint64_t snapshot_count;                 /* 已保存快照数 */
    time_t start_time;                       /* 启动时间（uptime） */
    volatile time_t heartbeat[STATE_THREAD_SLOTS]; /* 各线程心跳，槽位见 app.h */
} app_state_t;

app_state_t *state_get(void);
int state_init(void);
void state_shutdown(void);

void state_set_jpeg(const unsigned char *data, size_t len);
int state_copy_jpeg(unsigned char **out, size_t *out_len, uint64_t *seq);
void state_request_ai(void);
/* 请求保存一张快照，tag 作为文件名标签（如 "alarm"/"manual"） */
void state_request_snapshot(const char *tag);

/* 更新线程心跳（看门狗用），slot 见 app.h 的 APP_THREAD_* */
void state_heartbeat(int slot);

/* 追加一条告警历史（调用方必须持有 st->lock） */
void state_push_alarm_history_locked(const char *text);

#endif
