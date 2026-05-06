#ifndef MACROPAD_LED_BLINK_H
#define MACROPAD_LED_BLINK_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

typedef struct {
    bool active;
    bool led_on;
    uint32_t interval_ms;
    uint32_t blinks_remaining;
    absolute_time_t next_toggle_at;
} blink_sequence_t;

void led_set(bool enabled);
void blink_sequence_start(blink_sequence_t *sequence, uint32_t interval_ms,
                          uint32_t blink_count);
void blink_sequence_update(blink_sequence_t *sequence);

#endif
