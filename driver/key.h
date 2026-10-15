#ifndef GRANARY_DRIVER_KEY_H
#define GRANARY_DRIVER_KEY_H

#include <stdint.h>

#include <linux/input.h> /* KEY_1/KEY_2/KEY_3 等按键码默认值 */

/* 按键码（编译期覆盖：-DGRANARY_KEY1_CODE=...） */
#ifndef GRANARY_KEY1_CODE
#define GRANARY_KEY1_CODE KEY_1
#endif
#ifndef GRANARY_KEY2_CODE
#define GRANARY_KEY2_CODE KEY_2
#endif
#ifndef GRANARY_KEY3_CODE
#define GRANARY_KEY3_CODE KEY_3
#endif

#define KEY_MAX_EVENTS 32
#define KEY_STABLE_DEVICE_PATH "/dev/input/by-path/platform-keys-event"
#define KEY_DEVICE_PATH_FORMAT "/dev/input/event%d"
#define KEY_DEVICE_SCAN_COUNT 8
#define KEY_DEVICE_PATH_SIZE 64

typedef enum {
    KEY_EVENT_PRESS = 0,
    KEY_EVENT_RELEASE = 1,
    KEY_EVENT_HOLD = 2,
} key_event_type_t;

typedef struct {
    uint16_t code;
    key_event_type_t type;
    uint32_t value;
} key_event_t;

typedef struct {
    int fd;
    char device_path[KEY_DEVICE_PATH_SIZE];
    int initialized;
} key_device_t;

int key_init_auto(key_device_t *device);
int key_init(key_device_t *device, const char *device_path);
int key_read_event(key_device_t *device, key_event_t *event);
int key_is_pressed(key_device_t *device, uint16_t key_code);
int key_close(key_device_t *device);
const char *key_get_name(uint16_t key_code);

#endif
