#include "led_blink.h"

#include "pico/cyw43_arch.h"

void led_set(bool enabled) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, enabled ? 1 : 0);
}

void blink_sequence_start(blink_sequence_t *sequence, uint32_t interval_ms,
                          uint32_t blink_count) {
    if (blink_count == 0) {
        sequence->active = false;
        sequence->forever = false;
        sequence->led_on = false;
        led_set(false);
        return;
    }

    sequence->active = true;
    sequence->forever = false;
    sequence->led_on = true;
    sequence->interval_ms = interval_ms;
    sequence->blinks_remaining = blink_count;
    sequence->next_toggle_at = make_timeout_time_ms(interval_ms);

    led_set(true);
}

void blink_sequence_start_forever(blink_sequence_t *sequence,
                                  uint32_t interval_ms) {
    sequence->active = true;
    sequence->forever = true;
    sequence->led_on = true;
    sequence->interval_ms = interval_ms;
    sequence->blinks_remaining = 0;
    sequence->next_toggle_at = make_timeout_time_ms(interval_ms);

    led_set(true);
}

void blink_sequence_update(blink_sequence_t *sequence) {
    if (!sequence->active || !time_reached(sequence->next_toggle_at)) {
        return;
    }

    if (sequence->led_on) {
        sequence->led_on = false;
        led_set(false);

        if (!sequence->forever) {
            sequence->blinks_remaining--;
            if (sequence->blinks_remaining == 0) {
                sequence->active = false;
                return;
            }
        }
    } else {
        sequence->led_on = true;
        led_set(true);
    }

    sequence->next_toggle_at = make_timeout_time_ms(sequence->interval_ms);
}
