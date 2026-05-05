#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define BLINK_INTERVAL_MS 1000

int main(void) {
    stdio_init_all();

    if (cyw43_arch_init() != 0) {
        return 1;
    }

    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(BLINK_INTERVAL_MS);

        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(BLINK_INTERVAL_MS);
    }
}
