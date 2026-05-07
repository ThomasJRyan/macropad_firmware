#ifndef MACROPAD_APP_CONFIG_H
#define MACROPAD_APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define APP_CONFIG_WIFI_SSID_MAX 32u
#define APP_CONFIG_WIFI_PASSWORD_MAX 63u
#define APP_CONFIG_BUTTON_COUNT 2u
#define APP_CONFIG_ACTION_URL_MAX 128u
#define APP_CONFIG_ACTION_BODY_MAX 512u

typedef enum {
    APP_CONFIG_ACTION_DISABLED = 0,
    APP_CONFIG_ACTION_GET = 1,
    APP_CONFIG_ACTION_POST = 2,
} app_config_action_method_t;

typedef struct {
    app_config_action_method_t method;
    char url[APP_CONFIG_ACTION_URL_MAX + 1u];
    char body[APP_CONFIG_ACTION_BODY_MAX + 1u];
} app_config_button_action_t;

typedef struct {
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    app_config_button_action_t button_actions[APP_CONFIG_BUTTON_COUNT];
} app_config_t;

void app_config_init(void);
app_config_t app_config_get(void);
bool app_config_save(const app_config_t *config);
bool app_config_validate(const app_config_t *config);
bool app_config_has_wifi_credentials(const app_config_t *config);
const char *app_config_action_method_name(app_config_action_method_t method);

#endif
