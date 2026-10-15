#include "display.h"

#include <stdio.h>

void display_refresh(const app_state_t *st)
{
    if (st == NULL) {
        return;
    }
    printf("\n======== Granary Monitor ========\n");
    printf("Mode: %s | Mute: %s\n",
           st->inspection_running ? "RUNNING" : "PAUSED",
           st->buzzer_muted ? "yes" : "no");
    printf("U=%.2f V  I=%.2f A  P=%.2f W  (ADC=%.2f V)\n",
           st->supply_voltage_v, st->current_a, st->power_w,
           st->adc_voltage_v);
    printf("T=%.1f C  RH=%.1f %%  Lux=%.0f\n", st->temperature_c,
           st->humidity_rh, st->illuminance);
    printf("PIR=%s  Flame=%s\n",
           st->pir_detected ? "YES" : "no",
           st->flame_detected ? "YES" : "no");
    printf("Alarms: E01=%d E02=%d E03=%d E04=%d E05=%d\n", st->e01, st->e02,
           st->e03, st->e04, st->e05);
    if (st->alarm_text[0] != '\0') {
        printf("Alarm: %s\n", st->alarm_text);
    }
    printf("AI: %s\n", st->ai_text);
    printf("=================================\n");
    fflush(stdout);
}

void display_framebuffer_stub(const app_state_t *st)
{
    (void)st;
    /* No portable LCD path on all FS-MP1A images; web overlay is primary. */
}
