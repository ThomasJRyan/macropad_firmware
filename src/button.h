#ifndef MACROPAD_BUTTON_H
#define MACROPAD_BUTTON_H

#include <stdbool.h>

#include "pico/stdlib.h"

typedef struct {
    uint pin;
    bool stable_pressed;
    bool sampled_pressed;
    absolute_time_t sample_changed_at;
} button_t;

void button_init(button_t *button, uint pin);
bool button_update(button_t *button);

#endif
