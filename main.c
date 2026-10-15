#include "app/app.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
    
int main(void)
{
    pthread_t threads[APP_THREAD_COUNT];
    void *(*const thread_functions[APP_THREAD_COUNT])(void *) = {
        app_voltage_thread,
        app_climate_thread,
        app_proximity_thread,
    };
    size_t created_count = 0;
    size_t i;
    int result;
    int exit_status = 0;

    for (i = 0; i < APP_THREAD_COUNT; i++) {
        result = pthread_create(&threads[i], NULL, thread_functions[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to create application thread: %s\n",
                    strerror(result));
            app_request_stop();
            exit_status = 1;
            break;
        }
        created_count++;
    }

    for (i = 0; i < created_count; i++) {
        result = pthread_join(threads[i], NULL);
        if (result != 0) {
            fprintf(stderr, "Failed to join application thread: %s\n",
                    strerror(result));
            exit_status = 1;
        }
    }

    return exit_status;
}
