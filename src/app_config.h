#ifndef MACROPAD_APP_CONFIG_H
#define MACROPAD_APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_CONFIG_WIFI_SSID_MAX 32u
#define APP_CONFIG_WIFI_PASSWORD_MAX 63u
#define APP_CONFIG_BUTTON_COUNT 6u
#define APP_CONFIG_MDNS_HOSTNAME_MAX 32u
#define APP_CONFIG_DEFAULT_MDNS_HOSTNAME "macropad"
#define APP_CONFIG_ACTION_URL_COUNT_MAX 3u
#define APP_CONFIG_ACTION_URL_MAX 128u
#define APP_CONFIG_ACTION_BODY_MAX 384u
#define APP_CONFIG_ACTION_CONTENT_TYPE_MAX 64u
#define APP_CONFIG_ACTION_HEADERS_MAX 256u

typedef enum {
    APP_CONFIG_ACTION_DISABLED = 0,
    APP_CONFIG_ACTION_GET = 1,
    APP_CONFIG_ACTION_POST = 2,
} app_config_action_method_t;

typedef enum {
    APP_CONFIG_ACTION_TRIGGER_BURST = 0,
    APP_CONFIG_ACTION_TRIGGER_ROUND_ROBIN = 1,
} app_config_action_trigger_mode_t;

typedef struct {
    app_config_action_method_t method;
    app_config_action_trigger_mode_t trigger_mode;
    uint8_t url_count;
    char urls[APP_CONFIG_ACTION_URL_COUNT_MAX]
             [APP_CONFIG_ACTION_URL_MAX + 1u];
    char body[APP_CONFIG_ACTION_BODY_MAX + 1u];
    char content_type[APP_CONFIG_ACTION_CONTENT_TYPE_MAX + 1u];
    char headers[APP_CONFIG_ACTION_HEADERS_MAX + 1u];
} app_config_button_action_t;

typedef struct {
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    char mdns_hostname[APP_CONFIG_MDNS_HOSTNAME_MAX + 1u];
    app_config_button_action_t button_actions[APP_CONFIG_BUTTON_COUNT];
} app_config_t;

void app_config_init(void);
app_config_t app_config_default(void);
app_config_button_action_t app_config_default_button_action(void);
app_config_t app_config_get(void);
bool app_config_get_button_action(size_t button_index,
                                  app_config_button_action_t *action);
bool app_config_save(const app_config_t *config);
bool app_config_validate(const app_config_t *config);
bool app_config_has_wifi_credentials(const app_config_t *config);
const char *app_config_action_method_name(app_config_action_method_t method);
const char *app_config_action_trigger_mode_name(
    app_config_action_trigger_mode_t trigger_mode);
uint8_t app_config_button_pin(size_t button_index);

#endif
