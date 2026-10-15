#include "app.h"
#include "http_server.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    pthread_t threads[APP_THREAD_COUNT];
    void *(*const thread_functions[APP_THREAD_COUNT])(void *) = {
        app_sensor_thread,
        app_key_thread,
        app_camera_thread,
        app_alarm_thread,
        app_ai_thread,
        http_server_thread,
    };
    size_t created_count = 0;
    size_t i;
    int result;
    int exit_status = 0;

    if (app_init() < 0) {
        fprintf(stderr, "app_init failed\n");
        return 1;
    }

    for (i = 0; i < APP_THREAD_COUNT; i++) {
        result = pthread_create(&threads[i], NULL, thread_functions[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to create thread: %s\n", strerror(result));
            app_request_stop();
            exit_status = 1;
            break;
        }
        created_count++;
    }

    for (i = 0; i < created_count; i++) {
        result = pthread_join(threads[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to join thread: %s\n", strerror(result));
            exit_status = 1;
        }
    }

    app_shutdown();
    return exit_status;
}
