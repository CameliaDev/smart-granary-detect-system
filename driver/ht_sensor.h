#ifndef HT_SENSOR_H
#define HT_SENSOR_H

#define HT_SENSOR_MARKER "in_humidityrelative_raw"
#define HT_SENSOR_ERROR_VALUE (-999.0f)
#define HT_SENSOR_TEMPERATURE_RAW "in_temp_raw"
#define HT_SENSOR_TEMPERATURE_OFFSET "in_temp_offset"
#define HT_SENSOR_TEMPERATURE_SCALE "in_temp_scale"
#define HT_SENSOR_HUMIDITY_RAW "in_humidityrelative_raw"
#define HT_SENSOR_HUMIDITY_OFFSET "in_humidityrelative_offset"
#define HT_SENSOR_HUMIDITY_SCALE "in_humidityrelative_scale"

float ht_read_temperature(void);
float ht_read_humidity(void);

#endif
