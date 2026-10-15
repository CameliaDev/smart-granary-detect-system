#include "voltage_sensor.h"
#include "file.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

static int read_float_file(const char *path, float *value)
{
    char buf[64];
    char *end;
    float parsed;

    if (read_file(path, buf, sizeof(buf)) < 0) {
        return -1;
    }

    errno = 0;
    parsed = strtof(buf, &end);
    if (end == buf || errno == ERANGE) {
        return -1;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end++;
    }
    if (*end != '\0') {
        return -1;
    }

    *value = parsed;
    return 0;
}

int voltage_read(Voltage_Channel channel, float *millivolts)
{
    char directory[128];
    char path[192];
    float raw;
    float scale = VOLTAGE_SENSOR_DEFAULT_SCALE_MV;
    float offset = 0.0f;
    int path_length;

    if (millivolts == NULL || channel < VOLTAGE_CHANNEL_0 ||
        channel >= VOLTAGE_CHANNEL_MAX ||
        find_iio_device(VOLTAGE_SENSOR_RAW_MARKER,
                        directory, (int)sizeof(directory)) < 0) {
        return -1;
    }

    path_length = snprintf(path, sizeof(path), VOLTAGE_SENSOR_RAW_PATH_FORMAT,
                           directory, (int)channel);
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        read_float_file(path, &raw) < 0) {
        return -1;
    }

    path_length = snprintf(path, sizeof(path),
                           VOLTAGE_SENSOR_SCALE_PATH_FORMAT, directory);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return -1;
    }
    if (read_float_file(path, &scale) < 0) {
        scale = VOLTAGE_SENSOR_DEFAULT_SCALE_MV;
    }

    path_length = snprintf(path, sizeof(path),
                           VOLTAGE_SENSOR_OFFSET_PATH_FORMAT, directory);
    if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
        return -1;
    }
    if (read_float_file(path, &offset) < 0) {
        offset = 0.0f;
    }

    *millivolts = (raw + offset) * scale;
    return 0;
}

int current_read(float *millivolts)
{
    return voltage_read(VOLTAGE_CHANNEL_0, millivolts);
}

int voltage_sensor_read(float *millivolts)
{
    return voltage_read(VOLTAGE_CHANNEL_1, millivolts);
}
