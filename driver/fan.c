#include "fan.h"
#include "file.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int find_fan_pwm_path(char *path, size_t path_size)
{
    char name_path[96];
    char name[64];
    int i;
    int length;

    for (i = 0; i < FAN_HWMON_DEVICE_COUNT; i++) {
        length = snprintf(name_path, sizeof(name_path),
                          FAN_HWMON_NAME_PATH_FORMAT, i);
        if (length < 0 || (size_t)length >= sizeof(name_path)) {
            return -1;
        }
        if (access(name_path, R_OK) != 0 ||
            read_file(name_path, name, sizeof(name)) < 0) {
            continue;
        }
        if (strncmp(name, FAN_NAME_PREFIX, strlen(FAN_NAME_PREFIX)) != 0) {
            continue;
        }

        length = snprintf(path, path_size, FAN_PWM_PATH_FORMAT, i);
        return (length < 0 || (size_t)length >= path_size) ? -1 : 0;
    }

    return -1;
}

int fan_set(unsigned int pwm)
{
    char path[96];
    char value[16];
    int length;

    if (pwm > FAN_PWM_MAX) {
        return -1;
    }
    if (find_fan_pwm_path(path, sizeof(path)) < 0) {
        return -1;
    }

    length = snprintf(value, sizeof(value), "%u", pwm);
    if (length < 0 || (size_t)length >= sizeof(value)) {
        return -1;
    }
    return write_file(path, value);
}
