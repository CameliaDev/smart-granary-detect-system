#define _POSIX_C_SOURCE 199309L

#include "delay.h"

#include <errno.h>
#include <time.h>

static void delay_time(time_t seconds, long nanoseconds)
{
    struct timespec remaining;

    remaining.tv_sec = seconds;
    remaining.tv_nsec = nanoseconds;
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {
    }
}

void delay_ms(unsigned int milliseconds)
{
    delay_time((time_t)(milliseconds / 1000U),
               (long)(milliseconds % 1000U) * 1000000L);
}

void delay_us(unsigned int microseconds)
{
    delay_time((time_t)(microseconds / 1000000U),
               (long)(microseconds % 1000000U) * 1000L);
}
