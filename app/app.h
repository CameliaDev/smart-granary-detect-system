#ifndef APP_H
#define APP_H

#define APP_THREAD_COUNT 3

void app_request_stop(void);
void *app_voltage_thread(void *argument);
void *app_climate_thread(void *argument);
void *app_proximity_thread(void *argument);

#endif
