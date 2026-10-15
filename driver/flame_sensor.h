#ifndef FLAME_SENSOR_H
#define FLAME_SENSOR_H

/*
 * Digital flame sensor on FS-MP1A. Override path:
 *   -DFLAME_SENSOR_SYSFS_PATH=\"/sys/class/gpio/gpioXX/value\"
 * Many modules are active-low (0 = flame). Default FLAME_ACTIVE_LOW=1.
 */
#ifndef FLAME_SENSOR_SYSFS_PATH
#define FLAME_SENSOR_SYSFS_PATH "/sys/class/gpio/gpio116/value"
#endif

#ifndef FLAME_ACTIVE_LOW
#define FLAME_ACTIVE_LOW 1
#endif

/** @return 1 flame, 0 clear, -1 read error / missing device */
int flame_sensor_read(void);

#endif
