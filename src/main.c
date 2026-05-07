#include "app_config.h"
#include "button.h"
#include "network.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "rest_client.h"

#include <stdio.h>

#define BUTTON_0_PIN 5
#define BUTTON_1_PIN 6

#define BUTTON_POLL_INTERVAL_MS 5

int main(void) {
    stdio_init_all();

    printf("boot: macropad firmware starting\n");
    fflush(stdout);

    app_config_init();
    const app_config_t boot_config = app_config_get();
    printf("boot: config wifi_configured=%s ssid='%s'\n",
           app_config_has_wifi_credentials(&boot_config) ? "true" : "false",
           boot_config.wifi_ssid);
    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        printf("boot: button%lu method=%s url='%s'\n", (unsigned long)i,
               app_config_action_method_name(
                   boot_config.button_actions[i].method),
               boot_config.button_actions[i].url);
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

    button_t button_0;
    button_t button_1;

    button_init(&button_0, BUTTON_0_PIN);
    button_init(&button_1, BUTTON_1_PIN);
    printf("boot: buttons initialized button0=GP%u button1=GP%u\n",
           BUTTON_0_PIN, BUTTON_1_PIN);

    printf("boot: entering main loop\n");

    while (true) {
        if (button_update(&button_0)) {
            printf("main: button0 GP%u pressed\n", BUTTON_0_PIN);
            rest_client_trigger(0);
        }

        if (button_update(&button_1)) {
            printf("main: button1 GP%u pressed\n", BUTTON_1_PIN);
            rest_client_trigger(1);
        }

        network_debug_poll();

        sleep_ms(BUTTON_POLL_INTERVAL_MS);
    }
}
