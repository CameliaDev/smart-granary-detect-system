#define _POSIX_C_SOURCE 199309L

#include "my_timer.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    timer_t timer;
    unsigned int period_ms;
    int created;
    int running;
    Timer_Callback callback;
} Timer_Info;

static Timer_Info timers[MY_TIMER_MAX];
static pthread_mutex_t timers_mutex = PTHREAD_MUTEX_INITIALIZER;

static void timer_handler(union sigval value)
{
    Timer_Info *timer = value.sival_ptr;
    Timer_Callback callback = NULL;

    if (pthread_mutex_lock(&timers_mutex) != 0) {
        return;
    }
    if (timer >= timers && timer < timers + MY_TIMER_MAX &&
        timer->created && timer->running) {
        callback = timer->callback;
    }
    pthread_mutex_unlock(&timers_mutex);

    if (callback != NULL) {
        callback();
    }
}

static void make_timer_spec(unsigned int period_ms, struct itimerspec *spec)
{
    memset(spec, 0, sizeof(*spec));
    spec->it_value.tv_sec = (time_t)(period_ms / 1000U);
    spec->it_value.tv_nsec = (long)(period_ms % 1000U) * 1000000L;
    spec->it_interval = spec->it_value;
}

Timer_Handle my_timer_create(int period_ms, Timer_Callback callback)
{
    struct sigevent event;
    struct itimerspec spec;
    int slot;

    if (period_ms <= 0 || callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (pthread_mutex_lock(&timers_mutex) != 0) {
        return -1;
    }

    for (slot = 0; slot < MY_TIMER_MAX; slot++) {
        if (!timers[slot].created) {
            break;
        }
    }
    if (slot == MY_TIMER_MAX) {
        pthread_mutex_unlock(&timers_mutex);
        errno = ENOSPC;
        return -1;
    }

    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_THREAD;
    event.sigev_notify_function = timer_handler;
    event.sigev_value.sival_ptr = &timers[slot];

    if (timer_create(CLOCK_MONOTONIC, &event, &timers[slot].timer) < 0) {
        perror("timer_create failed");
        pthread_mutex_unlock(&timers_mutex);
        return -1;
    }

    timers[slot].period_ms = (unsigned int)period_ms;
    timers[slot].callback = callback;
    timers[slot].created = 1;
    timers[slot].running = 1;
    make_timer_spec(period_ms, &spec);
    if (timer_settime(timers[slot].timer, 0, &spec, NULL) < 0) {
        perror("timer_settime failed");
        timer_delete(timers[slot].timer);
        memset(&timers[slot], 0, sizeof(timers[slot]));
        pthread_mutex_unlock(&timers_mutex);
        return -1;
    }

    pthread_mutex_unlock(&timers_mutex);
    return slot;
}

int my_timer_start(Timer_Handle handle)
{
    struct itimerspec spec;

    if (handle < 0 || handle >= MY_TIMER_MAX ||
        pthread_mutex_lock(&timers_mutex) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!timers[handle].created) {
        pthread_mutex_unlock(&timers_mutex);
        errno = EINVAL;
        return -1;
    }

    make_timer_spec(timers[handle].period_ms, &spec);
    timers[handle].running = 1;
    if (timer_settime(timers[handle].timer, 0, &spec, NULL) < 0) {
        timers[handle].running = 0;
        perror("timer_settime failed");
        pthread_mutex_unlock(&timers_mutex);
        return -1;
    }

    pthread_mutex_unlock(&timers_mutex);
    return 0;
}

int my_timer_stop(Timer_Handle handle)
{
    struct itimerspec spec;

    if (handle < 0 || handle >= MY_TIMER_MAX ||
        pthread_mutex_lock(&timers_mutex) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!timers[handle].created) {
        pthread_mutex_unlock(&timers_mutex);
        errno = EINVAL;
        return -1;
    }

    memset(&spec, 0, sizeof(spec));
    if (timer_settime(timers[handle].timer, 0, &spec, NULL) < 0) {
        perror("timer_settime failed");
        pthread_mutex_unlock(&timers_mutex);
        return -1;
    }
    timers[handle].running = 0;

    pthread_mutex_unlock(&timers_mutex);
    return 0;
}

int my_timer_destroy(Timer_Handle handle)
{
    struct itimerspec spec;

    if (handle < 0 || handle >= MY_TIMER_MAX ||
        pthread_mutex_lock(&timers_mutex) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (!timers[handle].created) {
        pthread_mutex_unlock(&timers_mutex);
        errno = EINVAL;
        return -1;
    }

    memset(&spec, 0, sizeof(spec));
    if (timer_settime(timers[handle].timer, 0, &spec, NULL) < 0) {
        perror("timer_settime failed");
        pthread_mutex_unlock(&timers_mutex);
        return -1;
    }
    timers[handle].running = 0;
    if (timer_delete(timers[handle].timer) < 0) {
        perror("timer_delete failed");
        pthread_mutex_unlock(&timers_mutex);
        return -1;
    }
    memset(&timers[handle], 0, sizeof(timers[handle]));

    pthread_mutex_unlock(&timers_mutex);
    return 0;
}
