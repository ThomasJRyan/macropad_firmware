#ifndef MACROPAD_APP_CONFIG_H
#define MACROPAD_APP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t blink_count;
    uint32_t frequency_tenths;
} app_config_t;

void app_config_init(void);
app_config_t app_config_get(void);
bool app_config_save(const app_config_t *config);
bool app_config_validate(const app_config_t *config);
uint32_t app_config_interval_ms(const app_config_t *config);

#endif
