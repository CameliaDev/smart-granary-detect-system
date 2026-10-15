#include "flame_sensor.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int flame_sensor_read(void)
{
    char buf[32];
    long value;
    char *end;

    if (access(FLAME_SENSOR_SYSFS_PATH, R_OK) != 0) {
        return -1;
    }
    if (read_file(FLAME_SENSOR_SYSFS_PATH, buf, (int)sizeof(buf)) < 0) {
        return -1;
    }

    value = strtol(buf, &end, 10);
    if (end == buf) {
        return -1;
    }

#if FLAME_ACTIVE_LOW
    return value == 0 ? 1 : 0;
#else
    return value != 0 ? 1 : 0;
#endif
}
