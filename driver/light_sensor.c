#include "light_sensor.h"
#include "file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static float read_iio_attribute(const char *attribute)
{
    char directory[128];
    char path[192];
    char buf[64];
    char *end;
    float value;
    int path_length;

    if (find_iio_device(attribute, directory, (int)sizeof(directory)) < 0) {
        return LIGHT_SENSOR_ERROR_VALUE;
    }

    path_length = snprintf(path, sizeof(path), "%s/%s", directory, attribute);
    if (path_length < 0 || (size_t)path_length >= sizeof(path) ||
        read_file(path, buf, sizeof(buf)) < 0) {
        return LIGHT_SENSOR_ERROR_VALUE;
    }

    errno = 0;
    value = strtof(buf, &end);
    if (end == buf || errno == ERANGE) {
        return LIGHT_SENSOR_ERROR_VALUE;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end++;
    }
    if (*end != '\0') {
        return LIGHT_SENSOR_ERROR_VALUE;
    }
    return value;
}

float light_read(void)
{
    return read_iio_attribute(LIGHT_SENSOR_VISIBLE_ATTRIBUTE);
}

float light_read_ir(void)
{
    return read_iio_attribute(LIGHT_SENSOR_IR_ATTRIBUTE);
}

float light_read_proximity(void)
{
    return read_iio_attribute(LIGHT_SENSOR_PROXIMITY_ATTRIBUTE);
}
