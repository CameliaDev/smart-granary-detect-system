#include "ht_sensor.h"
#include "file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

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

static float read_channel(const char *raw_name, const char *offset_name,
                          const char *scale_name)
{
    char directory[128];
    char path[192];
    float raw;
    float offset;
    float scale;
    int path_length;

    if (find_iio_device(HT_SENSOR_MARKER,
                        directory, (int)sizeof(directory)) < 0) {
        return HT_SENSOR_ERROR_VALUE;
    }

    path_length = snprintf(path, sizeof(path), "%s/%s", directory, raw_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        read_float_file(path, &raw) < 0) {
        return HT_SENSOR_ERROR_VALUE;
    }
    path_length = snprintf(path, sizeof(path), "%s/%s", directory, offset_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        read_float_file(path, &offset) < 0) {
        return HT_SENSOR_ERROR_VALUE;
    }
    path_length = snprintf(path, sizeof(path), "%s/%s", directory, scale_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        read_float_file(path, &scale) < 0) {
        return HT_SENSOR_ERROR_VALUE;
    }

    return (raw + offset) * scale / 1000.0f;
}

float ht_read_temperature(void)
{
    return read_channel(HT_SENSOR_TEMPERATURE_RAW,
                        HT_SENSOR_TEMPERATURE_OFFSET,
                        HT_SENSOR_TEMPERATURE_SCALE);
}

float ht_read_humidity(void)
{
    return read_channel(HT_SENSOR_HUMIDITY_RAW,
                        HT_SENSOR_HUMIDITY_OFFSET,
                        HT_SENSOR_HUMIDITY_SCALE);
}
