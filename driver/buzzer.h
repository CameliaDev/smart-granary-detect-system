#ifndef BUZZER_H
#define BUZZER_H

#define BUZZER_DEVICE_PATH "/dev/input/by-path/platform-beeper-event"
#define BUZZER_OFF_FREQUENCY 0U

int buzzer_set(unsigned int frequency_hz);
int buzzer_play_music(void);

#endif
