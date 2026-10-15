#ifndef FAN_H
#define FAN_H

#define FAN_HWMON_NAME_PATH_FORMAT "/sys/class/hwmon/hwmon%d/name"
#define FAN_PWM_PATH_FORMAT "/sys/class/hwmon/hwmon%d/pwm1"
#define FAN_HWMON_DEVICE_COUNT 8
#define FAN_NAME_PREFIX "pwmfan"
#define FAN_PWM_MAX 255U

int fan_set(unsigned int pwm);

#endif
