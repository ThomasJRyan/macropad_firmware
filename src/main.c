#include "app_config.h"
#include "blink_request.h"
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
#define STATUS_BLINK_INTERVAL_MS 100
#define STATUS_SUCCESS_BLINK_COUNT 3

static void blink_sequence_start_from_config(blink_sequence_t *sequence) {
    const app_config_t config = app_config_get();
    const uint32_t interval_ms = app_config_interval_ms(&config);

    if (config.blink_count > 0 && interval_ms > 0) {
        blink_sequence_start(sequence, interval_ms, config.blink_count);
    }
}

int main(void) {
    stdio_init_all();
    app_config_init();
    blink_request_init();

    if (cyw43_arch_init() != 0) {
        return 1;
    }

    led_set(false);

    const network_start_result_t network = network_start();
    if (network.err != ERR_OK) {
        return 1;
    }

    button_t fast_button;
    button_t slow_button;
    blink_sequence_t led_sequence = {0};

    button_init(&fast_button, BUTTON_FAST_PIN);
    button_init(&slow_button, BUTTON_SLOW_PIN);

    bool saved_boot_blink_pending = false;
    if (network.status == NETWORK_START_STATION_CONNECTED) {
        blink_sequence_start(&led_sequence, STATUS_BLINK_INTERVAL_MS,
                             STATUS_SUCCESS_BLINK_COUNT);
        saved_boot_blink_pending = true;
    } else if (network.status == NETWORK_START_STATION_FAILED) {
        blink_sequence_start_forever(&led_sequence, STATUS_BLINK_INTERVAL_MS);
    } else {
        blink_sequence_start_from_config(&led_sequence);
    }

    while (true) {
        const bool web_blink_requested = blink_request_take_web();
        const bool fast_pressed = button_update(&fast_button);
        const bool slow_pressed = button_update(&slow_button);

        if (web_blink_requested) {
            blink_sequence_start_from_config(&led_sequence);
        } else if (fast_pressed) {
            blink_sequence_start(&led_sequence, FAST_BLINK_INTERVAL_MS,
                                 BLINK_COUNT);
        } else if (slow_pressed) {
            blink_sequence_start(&led_sequence, SLOW_BLINK_INTERVAL_MS,
                                 BLINK_COUNT);
        }

        blink_sequence_update(&led_sequence);
        if (saved_boot_blink_pending && !led_sequence.active) {
            saved_boot_blink_pending = false;
            blink_sequence_start_from_config(&led_sequence);
        } else if (network.status == NETWORK_START_STATION_FAILED &&
                   !led_sequence.active) {
            blink_sequence_start_forever(&led_sequence,
                                         STATUS_BLINK_INTERVAL_MS);
        }

        sleep_ms(BUTTON_POLL_INTERVAL_MS);
    }
}
