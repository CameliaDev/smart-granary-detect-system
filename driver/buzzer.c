#include "buzzer.h"
#include "delay.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int buzzer_set(unsigned int frequency_hz)
{
    struct input_event event;
    ssize_t written;
    int fd;

    if (frequency_hz > INT_MAX) {
        errno = EINVAL;
        return -1;
    }

    fd = open(BUZZER_DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        perror("open buzzer failed");
        return -1;
    }

    memset(&event, 0, sizeof(event));
    event.type = EV_SND;
    event.code = SND_TONE;
    event.value = (int)frequency_hz;

    do {
        written = write(fd, &event, sizeof(event));
    } while (written < 0 && errno == EINTR);

    if (written != (ssize_t)sizeof(event)) {
        if (written >= 0) {
            errno = EIO;
        }
        perror("write buzzer failed");
        close(fd);
        return -1;
    }
    if (close(fd) < 0) {
        perror("close buzzer failed");
        return -1;
    }

    return 0;
}

static int play_note(unsigned int frequency_hz, unsigned int duration_ms)
{
    if (buzzer_set(frequency_hz) < 0) {
        return -1;
    }
    delay_ms(duration_ms);
    if (buzzer_set(BUZZER_OFF_FREQUENCY) < 0) {
        return -1;
    }
    delay_ms(30);
    return 0;
}

int buzzer_play_music(void)
{
    static const unsigned int frequencies[] = {
        523, 523, 784, 784, 880, 880, 784,
        698, 698, 659, 659, 587, 587, 523,
        784, 784, 698, 698, 659, 659, 587,
        784, 784, 698, 698, 659, 659, 587,
        523, 523, 784, 784, 880, 880, 784,
        698, 698, 659, 659, 587, 587, 523
    };
    static const unsigned int durations[] = {
        300, 300, 300, 300, 300, 300, 600,
        300, 300, 300, 300, 300, 300, 600,
        300, 300, 300, 300, 300, 300, 600,
        300, 300, 300, 300, 300, 300, 600,
        300, 300, 300, 300, 300, 300, 600,
        300, 300, 300, 300, 300, 300, 600
    };
    size_t i;

    for (i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); i++) {
        if (play_note(frequencies[i], durations[i]) < 0) {
            buzzer_set(BUZZER_OFF_FREQUENCY);
            return -1;
        }
    }
    return buzzer_set(BUZZER_OFF_FREQUENCY);
}
