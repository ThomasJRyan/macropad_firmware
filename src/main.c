#include "button.h"
#include "led_blink.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define BUTTON_FAST_PIN 5
#define BUTTON_SLOW_PIN 6

#define BUTTON_POLL_INTERVAL_MS 5

#define BLINK_COUNT 5
#define FAST_BLINK_INTERVAL_MS 250
#define SLOW_BLINK_INTERVAL_MS 1000

int main(void) {
    stdio_init_all();

    if (cyw43_arch_init() != 0) {
        return 1;
    }

    led_set(false);

    if (network_start() != ERR_OK) {
        return 1;
    }

    button_t fast_button;
    button_t slow_button;
    blink_sequence_t led_sequence = {0};

    button_init(&fast_button, BUTTON_FAST_PIN);
    button_init(&slow_button, BUTTON_SLOW_PIN);

    while (true) {
        const bool fast_pressed = button_update(&fast_button);
        const bool slow_pressed = button_update(&slow_button);

        if (fast_pressed) {
            blink_sequence_start(&led_sequence, FAST_BLINK_INTERVAL_MS,
                                 BLINK_COUNT);
        } else if (slow_pressed) {
            blink_sequence_start(&led_sequence, SLOW_BLINK_INTERVAL_MS,
                                 BLINK_COUNT);
        }

        blink_sequence_update(&led_sequence);
        sleep_ms(BUTTON_POLL_INTERVAL_MS);
    }
}
