#ifndef VOLTAGE_SENSOR_H
#define VOLTAGE_SENSOR_H

#define VOLTAGE_SENSOR_RAW_MARKER "in_voltage0_raw"
#define VOLTAGE_SENSOR_RAW_PATH_FORMAT "%s/in_voltage%d_raw"
#define VOLTAGE_SENSOR_SCALE_PATH_FORMAT "%s/in_voltage_scale"
#define VOLTAGE_SENSOR_OFFSET_PATH_FORMAT "%s/in_voltage_offset"
#define VOLTAGE_SENSOR_DEFAULT_SCALE_MV (3300.0f / 65536.0f)

typedef enum {
    VOLTAGE_CHANNEL_0 = 0,
    VOLTAGE_CHANNEL_1,
    VOLTAGE_CHANNEL_MAX,
} Voltage_Channel;

int voltage_read(Voltage_Channel channel, float *millivolts);
int current_read(float *millivolts);
int voltage_sensor_read(float *millivolts);

#endif
