#ifndef APP_H
#define APP_H

#define APP_THREAD_COUNT 6

void app_request_stop(void);
int app_init(void);
void app_shutdown(void);

void *app_sensor_thread(void *argument);
void *app_key_thread(void *argument);
void *app_camera_thread(void *argument);
void *app_alarm_thread(void *argument);
void *app_ai_thread(void *argument);
/* http_server_thread declared in http_server.h */

#endif
