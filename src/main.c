#include "app_config.h"
#include "blink_request.h"
#include "button.h"
#include "led_blink.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include <stdio.h>

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

    printf("boot: macropad firmware starting\n");
    fflush(stdout);

    app_config_init();
    const app_config_t boot_config = app_config_get();
    printf("boot: config blink_count=%lu frequency_tenths=%lu "
           "wifi_configured=%s ssid='%s'\n",
           (unsigned long)boot_config.blink_count,
           (unsigned long)boot_config.frequency_tenths,
           app_config_has_wifi_credentials(&boot_config) ? "true" : "false",
           boot_config.wifi_ssid);

    blink_request_init();
    printf("boot: blink request queue initialized\n");

    if (cyw43_arch_init() != 0) {
        printf("boot: cyw43_arch_init failed\n");
        return 1;
    }
    printf("boot: cyw43_arch_init ok\n");

    led_set(false);
    printf("boot: onboard LED initialized off\n");

    const network_start_result_t network = network_start();
    printf("boot: network_start err=%d mode=%d status=%d\n",
           (int)network.err, (int)network.mode, (int)network.status);
    if (network.err != ERR_OK) {
        printf("boot: network startup failed, exiting main\n");
        return 1;
    }

    button_t fast_button;
    button_t slow_button;
    blink_sequence_t led_sequence = {0};

    button_init(&fast_button, BUTTON_FAST_PIN);
    button_init(&slow_button, BUTTON_SLOW_PIN);
    printf("boot: buttons initialized fast=GP%u slow=GP%u\n",
           BUTTON_FAST_PIN, BUTTON_SLOW_PIN);

    bool saved_boot_blink_pending = false;
    if (network.status == NETWORK_START_STATION_CONNECTED) {
        printf("boot: station connected, playing success blink\n");
        blink_sequence_start(&led_sequence, STATUS_BLINK_INTERVAL_MS,
                             STATUS_SUCCESS_BLINK_COUNT);
        saved_boot_blink_pending = true;
    } else if (network.status == NETWORK_START_STATION_FAILED) {
        printf("boot: station failed, starting failure blink\n");
        blink_sequence_start_forever(&led_sequence, STATUS_BLINK_INTERVAL_MS);
    } else {
        printf("boot: setup AP mode, applying saved blink config\n");
        blink_sequence_start_from_config(&led_sequence);
    }

    printf("boot: entering main loop\n");

    while (true) {
        const bool web_blink_requested = blink_request_take_web();
        const bool fast_pressed = button_update(&fast_button);
        const bool slow_pressed = button_update(&slow_button);

        if (web_blink_requested) {
            printf("main: web blink request received\n");
            blink_sequence_start_from_config(&led_sequence);
        } else if (fast_pressed) {
            printf("main: fast button pressed\n");
            blink_sequence_start(&led_sequence, FAST_BLINK_INTERVAL_MS,
                                 BLINK_COUNT);
        } else if (slow_pressed) {
            printf("main: slow button pressed\n");
            blink_sequence_start(&led_sequence, SLOW_BLINK_INTERVAL_MS,
                                 BLINK_COUNT);
        }

        blink_sequence_update(&led_sequence);
        network_debug_poll();

        if (saved_boot_blink_pending && !led_sequence.active) {
            saved_boot_blink_pending = false;
            printf("main: success blink done, applying saved blink config\n");
            blink_sequence_start_from_config(&led_sequence);
        } else if (network.status == NETWORK_START_STATION_FAILED &&
                   !led_sequence.active) {
            printf("main: restarting station failure blink\n");
            blink_sequence_start_forever(&led_sequence,
                                         STATUS_BLINK_INTERVAL_MS);
        }

        sleep_ms(BUTTON_POLL_INTERVAL_MS);
    }
}
