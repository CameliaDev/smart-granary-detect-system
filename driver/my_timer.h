#ifndef MY_TIMER_H
#define MY_TIMER_H

#define MY_TIMER_MAX 8

typedef int Timer_Handle;
typedef void (*Timer_Callback)(void);

/* Creating a timer starts it immediately; period_ms must be positive. */
Timer_Handle my_timer_create(int period_ms, Timer_Callback callback);
int my_timer_start(Timer_Handle handle);
int my_timer_stop(Timer_Handle handle);
int my_timer_destroy(Timer_Handle handle);

#endif
