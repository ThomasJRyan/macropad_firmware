#include <stdbool.h>
#include <stdint.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define BUTTON_FAST_PIN 6
#define BUTTON_SLOW_PIN 7

#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_POLL_INTERVAL_MS 5

#define BLINK_COUNT 5
#define FAST_BLINK_INTERVAL_MS 250
#define SLOW_BLINK_INTERVAL_MS 1000

typedef struct {
    uint pin;
    bool stable_pressed;
    bool sampled_pressed;
    absolute_time_t sample_changed_at;
} button_t;

typedef struct {
    bool active;
    bool led_on;
    uint32_t interval_ms;
    uint32_t blinks_remaining;
    absolute_time_t next_toggle_at;
} blink_sequence_t;

static bool button_is_pressed(uint pin) {
    return !gpio_get(pin);
}

static void button_init(button_t *button, uint pin) {
    button->pin = pin;

    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);

    button->stable_pressed = button_is_pressed(pin);
    button->sampled_pressed = button->stable_pressed;
    button->sample_changed_at = get_absolute_time();
}

static bool button_update(button_t *button) {
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

static void led_set(bool enabled) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, enabled ? 1 : 0);
}

static void blink_start(blink_sequence_t *sequence, uint32_t interval_ms) {
    sequence->active = true;
    sequence->led_on = true;
    sequence->interval_ms = interval_ms;
    sequence->blinks_remaining = BLINK_COUNT;
    sequence->next_toggle_at = make_timeout_time_ms(interval_ms);

    led_set(true);
}

static void blink_update(blink_sequence_t *sequence) {
    if (!sequence->active || !time_reached(sequence->next_toggle_at)) {
        return;
    }

    if (sequence->led_on) {
        sequence->led_on = false;
        led_set(false);

        sequence->blinks_remaining--;
        if (sequence->blinks_remaining == 0) {
            sequence->active = false;
            return;
        }
    } else {
        sequence->led_on = true;
        led_set(true);
    }

    sequence->next_toggle_at = make_timeout_time_ms(sequence->interval_ms);
}

int main(void) {
    stdio_init_all();

    if (cyw43_arch_init() != 0) {
        return 1;
    }

    led_set(false);

    button_t fast_button;
    button_t slow_button;
    blink_sequence_t led_sequence = {0};

    button_init(&fast_button, BUTTON_FAST_PIN);
    button_init(&slow_button, BUTTON_SLOW_PIN);

    while (true) {
        const bool fast_pressed = button_update(&fast_button);
        const bool slow_pressed = button_update(&slow_button);

        if (fast_pressed) {
            blink_start(&led_sequence, FAST_BLINK_INTERVAL_MS);
        } else if (slow_pressed) {
            blink_start(&led_sequence, SLOW_BLINK_INTERVAL_MS);
        }

        blink_update(&led_sequence);
        sleep_ms(BUTTON_POLL_INTERVAL_MS);
    }
}
