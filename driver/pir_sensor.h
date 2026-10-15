#ifndef PIR_SENSOR_H
#define PIR_SENSOR_H

/*
 * Human IR / PIR on FS-MP1A. Path is board-dependent; override at compile time:
 *   -DPIR_SENSOR_SYSFS_PATH=\"/sys/class/gpio/gpioXX/value\"
 * Active-high by default (1 = person detected). Override with -DPIR_ACTIVE_LOW=1.
 */
#ifndef PIR_SENSOR_SYSFS_PATH
#define PIR_SENSOR_SYSFS_PATH "/sys/class/gpio/gpio115/value"
#endif

#ifndef PIR_ACTIVE_LOW
#define PIR_ACTIVE_LOW 0
#endif

/** @return 1 detected, 0 clear, -1 read error / missing device */
int pir_sensor_read(void);

#endif
