#include "state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static app_state_t g_state;

app_state_t *state_get(void)
{
    return &g_state;
}

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

void state_request_snapshot(void)
{
    pthread_mutex_lock(&g_state.lock);
    g_state.snapshot_request = 1;
    pthread_mutex_unlock(&g_state.lock);
}
