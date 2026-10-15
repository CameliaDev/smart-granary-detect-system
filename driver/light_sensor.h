#ifndef LIGHT_SENSOR_H
#define LIGHT_SENSOR_H

#define LIGHT_SENSOR_ERROR_VALUE (-1.0f)
#define LIGHT_SENSOR_VISIBLE_ATTRIBUTE "in_illuminance_input"
#define LIGHT_SENSOR_IR_ATTRIBUTE "in_illuminance_ir_raw"
#define LIGHT_SENSOR_PROXIMITY_ATTRIBUTE "in_proximity_raw"

float light_read(void);
float light_read_ir(void);
float light_read_proximity(void);

#endif
