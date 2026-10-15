#include "LED.h"
#include "file.h"

#include <stdio.h>

int ledcontrol(LED_Device_ID lednum, LED_Status ledstart)
{
    char path[64];
    int path_length;
    const char *brightness;

    if (lednum < LED_DEVICE_ID_1 || lednum >= LED_DEVICE_ID_MAX) {
        return -1;
    }

    if (ledstart == LED_OFF) {
        brightness = "0";
    } else if (ledstart == LED_ON) {
        brightness = "1";
    } else {
        return -1;
    }

    path_length = snprintf(path, sizeof(path), LED_BRIGHTNESS_PATH_FORMAT,
                           (int)lednum);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return -1;
    }

    return write_file(path, brightness);
}