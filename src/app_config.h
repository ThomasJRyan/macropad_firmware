#ifndef MACROPAD_APP_CONFIG_H
#define MACROPAD_APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define APP_CONFIG_WIFI_SSID_MAX 32u
#define APP_CONFIG_WIFI_PASSWORD_MAX 63u

typedef struct {
    uint32_t blink_count;
    uint32_t frequency_tenths;
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
} app_config_t;

void app_config_init(void);
app_config_t app_config_get(void);
bool app_config_save(const app_config_t *config);
bool app_config_validate(const app_config_t *config);
bool app_config_has_wifi_credentials(const app_config_t *config);
uint32_t app_config_interval_ms(const app_config_t *config);

#endif
