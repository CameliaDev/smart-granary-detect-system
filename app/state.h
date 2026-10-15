#ifndef STATE_H
#define STATE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define STATE_AI_TEXT_MAX 1024
#define STATE_ALARM_TEXT_MAX 256
#define STATE_OVERLAY_MAX 512

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

    /* AI */
    char ai_text[STATE_AI_TEXT_MAX];
    int ai_intrusion;
    int ai_mold;
    int ai_collapse;
    int ai_fire;
    int ai_request; /* set by KEY2 or periodic */
    uint64_t ai_seq;

    /* Latest JPEG for stream / snapshot */
    unsigned char *jpeg;
    size_t jpeg_len;
    size_t jpeg_cap;
    uint64_t jpeg_seq;
    int snapshot_request;

    /* Snapshot dir */
    char snapshot_dir[256];
} app_state_t;

app_state_t *state_get(void);
int state_init(void);
void state_shutdown(void);

void state_set_jpeg(const unsigned char *data, size_t len);
int state_copy_jpeg(unsigned char **out, size_t *out_len, uint64_t *seq);
void state_request_ai(void);
void state_request_snapshot(void);

#endif
