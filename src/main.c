#include "app_config.h"
#include "button.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "rest_client.h"

#include <stdio.h>

#define BUTTON_POLL_INTERVAL_MS 5

int main(void) {
    stdio_init_all();

    printf("boot: macropad firmware starting\n");
    fflush(stdout);

    app_config_init();
    const app_config_t boot_config = app_config_get();
    printf("boot: config wifi_configured=%s ssid='%s' mdns='%s.local'\n",
           app_config_has_wifi_credentials(&boot_config) ? "true" : "false",
           boot_config.wifi_ssid, boot_config.mdns_hostname);
    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        printf("boot: button%lu method=%s mode=%s urls=%u first_url='%s'\n",
               (unsigned long)i,
               app_config_action_method_name(
                   boot_config.button_actions[i].method),
               app_config_action_trigger_mode_name(
                   boot_config.button_actions[i].trigger_mode),
               (unsigned int)boot_config.button_actions[i].url_count,
               boot_config.button_actions[i].urls[0]);
    }

    if (cyw43_arch_init() != 0) {
        printf("boot: cyw43_arch_init failed\n");
        return 1;
    }
    printf("boot: cyw43_arch_init ok\n");

    const network_start_result_t network = network_start();
    printf("boot: network_start err=%d mode=%d status=%d\n",
           (int)network.err, (int)network.mode, (int)network.status);
    if (network.err != ERR_OK) {
        printf("boot: network startup failed, exiting main\n");
        return 1;
    }

    rest_client_init();

    button_t buttons[APP_CONFIG_BUTTON_COUNT];

    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        const uint8_t pin = app_config_button_pin(i);
        button_init(&buttons[i], pin);
        printf("boot: button%lu initialized GP%u\n", (unsigned long)i,
               (unsigned int)pin);
    }

    printf("boot: entering main loop\n");

    while (true) {
        for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
            if (button_update(&buttons[i])) {
                printf("main: button%lu GP%u pressed\n", (unsigned long)i,
                       (unsigned int)app_config_button_pin(i));
                rest_client_trigger(i);
            }
        }

        rest_client_poll();
        network_debug_poll();

        sleep_ms(BUTTON_POLL_INTERVAL_MS);
    }
}
