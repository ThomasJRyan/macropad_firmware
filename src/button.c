#include "button.h"

#define BUTTON_DEBOUNCE_MS 30

static bool button_is_pressed(uint pin) {
    return !gpio_get(pin);
}

void button_init(button_t *button, uint pin) {
    button->pin = pin;

    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    button->stable_pressed = button_is_pressed(pin);
    button->sampled_pressed = button->stable_pressed;
    button->sample_changed_at = get_absolute_time();
}

bool button_update(button_t *button) {
    const bool pressed = button_is_pressed(button->pin);

    if (pressed != button->sampled_pressed) {
        button->sampled_pressed = pressed;
        button->sample_changed_at = get_absolute_time();
        return false;
    }

    if (pressed == button->stable_pressed) {
        return false;
    }

    if (absolute_time_diff_us(button->sample_changed_at, get_absolute_time()) <
        BUTTON_DEBOUNCE_MS * 1000) {
        return false;
    }

    button->stable_pressed = pressed;
    return button->stable_pressed;
}
