#ifndef LED_H
#define LED_H

#define LED_BRIGHTNESS_PATH_FORMAT "/sys/class/leds/led%d/brightness"

typedef enum {
    LED_OFF = 0,
    LED_ON = 1,
} LED_Status;

typedef enum {
    LED_DEVICE_ID_1 = 1,
    LED_DEVICE_ID_2,
    LED_DEVICE_ID_3,
    LED_DEVICE_ID_MAX,
} LED_Device_ID;

/**
 * @brief Control an LED through its sysfs brightness interface.
 * @param lednum LED device ID (LED_DEVICE_ID_1 through LED_DEVICE_ID_3).
 * @param ledstart LED_OFF or LED_ON.
 * @return 0 on success, -1 on failure.
 */
int ledcontrol(LED_Device_ID lednum, LED_Status ledstart);

#endif
