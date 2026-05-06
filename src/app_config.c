#include "app_config.h"

#include <stddef.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "pico/critical_section.h"
#include "pico/flash.h"
#include "pico/platform.h"

#define CONFIG_MAGIC 0x4D504346u
#define CONFIG_VERSION 2u
#define CONFIG_VERSION_1 1u
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_MAX_BLINKS 100u
#define CONFIG_MAX_FREQUENCY_TENTHS 600u
#define CONFIG_FLASH_TIMEOUT_MS 1000u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t blink_count;
    uint32_t frequency_tenths;
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    uint32_t checksum;
} config_flash_record_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t blink_count;
    uint32_t frequency_tenths;
    uint32_t checksum;
} config_flash_record_v1_t;

typedef struct {
    uint8_t page[FLASH_PAGE_SIZE];
} config_flash_write_t;

static critical_section_t config_lock;
static app_config_t current_config;
extern char __flash_binary_end;

_Static_assert(sizeof(config_flash_record_t) <= FLASH_PAGE_SIZE,
               "config record must fit in one flash page");

static size_t bounded_string_length(const char *value, size_t max_length) {
    size_t length = 0;
    while (length <= max_length && value[length] != '\0') {
        length++;
    }

    return length;
}

static uint32_t checksum_bytes(const void *data, size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t checksum = 2166136261u;

    for (size_t i = 0; i < length; i++) {
        checksum ^= bytes[i];
        checksum *= 16777619u;
    }

    return checksum ^ 0xA5A55A5Au;
}

static uint32_t config_checksum(const config_flash_record_t *record) {
    return checksum_bytes(record, offsetof(config_flash_record_t, checksum));
}

static uint32_t config_v1_checksum(const config_flash_record_v1_t *record) {
    return record->magic ^ record->version ^ record->blink_count ^
           record->frequency_tenths ^ 0xA5A55A5Au;
}

static app_config_t config_default(void) {
    const app_config_t config = {
        .blink_count = 0,
        .frequency_tenths = 0,
        .wifi_ssid = "",
        .wifi_password = "",
    };
    return config;
}

bool app_config_validate(const app_config_t *config) {
    const size_t ssid_length =
        bounded_string_length(config->wifi_ssid, APP_CONFIG_WIFI_SSID_MAX);
    const size_t password_length = bounded_string_length(
        config->wifi_password, APP_CONFIG_WIFI_PASSWORD_MAX);

    return config->blink_count <= CONFIG_MAX_BLINKS &&
           config->frequency_tenths <= CONFIG_MAX_FREQUENCY_TENTHS &&
           ssid_length <= APP_CONFIG_WIFI_SSID_MAX &&
           password_length <= APP_CONFIG_WIFI_PASSWORD_MAX;
}

static bool config_record_valid(const config_flash_record_t *record) {
    app_config_t config = {
        .blink_count = record->blink_count,
        .frequency_tenths = record->frequency_tenths,
    };
    memcpy(config.wifi_ssid, record->wifi_ssid, sizeof(config.wifi_ssid));
    memcpy(config.wifi_password, record->wifi_password,
           sizeof(config.wifi_password));

    return record->magic == CONFIG_MAGIC &&
           record->version == CONFIG_VERSION &&
           record->checksum == config_checksum(record) &&
           app_config_validate(&config);
}

static bool config_record_v1_valid(const config_flash_record_v1_t *record) {
    const app_config_t config = {
        .blink_count = record->blink_count,
        .frequency_tenths = record->frequency_tenths,
        .wifi_ssid = "",
        .wifi_password = "",
    };

    return record->magic == CONFIG_MAGIC &&
           record->version == CONFIG_VERSION_1 &&
           record->checksum == config_v1_checksum(record) &&
           app_config_validate(&config);
}

static config_flash_record_t config_record_from_app(
    const app_config_t *config) {
    config_flash_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = CONFIG_MAGIC;
    record.version = CONFIG_VERSION;
    record.blink_count = config->blink_count;
    record.frequency_tenths = config->frequency_tenths;
    memcpy(record.wifi_ssid, config->wifi_ssid, sizeof(record.wifi_ssid));
    memcpy(record.wifi_password, config->wifi_password,
           sizeof(record.wifi_password));
    record.checksum = config_checksum(&record);
    return record;
}

static void __not_in_flash_func(config_flash_write)(void *param) {
    config_flash_write_t *write = (config_flash_write_t *)param;

    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, write->page, FLASH_PAGE_SIZE);
}

void app_config_init(void) {
    critical_section_init(&config_lock);
    hard_assert((uintptr_t)&__flash_binary_end - XIP_BASE <=
                CONFIG_FLASH_OFFSET);

    app_config_t config = config_default();
    const config_flash_record_t *stored =
        (const config_flash_record_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);

    if (config_record_valid(stored)) {
        config.blink_count = stored->blink_count;
        config.frequency_tenths = stored->frequency_tenths;
        memcpy(config.wifi_ssid, stored->wifi_ssid,
               sizeof(config.wifi_ssid));
        config.wifi_ssid[APP_CONFIG_WIFI_SSID_MAX] = '\0';
        memcpy(config.wifi_password, stored->wifi_password,
               sizeof(config.wifi_password));
        config.wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX] = '\0';
    } else {
        const config_flash_record_v1_t *stored_v1 =
            (const config_flash_record_v1_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
        if (config_record_v1_valid(stored_v1)) {
            config.blink_count = stored_v1->blink_count;
            config.frequency_tenths = stored_v1->frequency_tenths;
        }
    }

    critical_section_enter_blocking(&config_lock);
    current_config = config;
    critical_section_exit(&config_lock);
}

app_config_t app_config_get(void) {
    critical_section_enter_blocking(&config_lock);
    const app_config_t config = current_config;
    critical_section_exit(&config_lock);

    return config;
}

bool app_config_save(const app_config_t *config) {
    if (!app_config_validate(config)) {
        return false;
    }

    config_flash_write_t write;
    memset(&write, 0xff, sizeof(write));

    const config_flash_record_t record = config_record_from_app(config);
    memcpy(write.page, &record, sizeof(record));

    if (flash_safe_execute(config_flash_write, &write,
                           CONFIG_FLASH_TIMEOUT_MS) != PICO_OK) {
        return false;
    }

    critical_section_enter_blocking(&config_lock);
    current_config = *config;
    critical_section_exit(&config_lock);

    return true;
}

bool app_config_has_wifi_credentials(const app_config_t *config) {
    return config->wifi_ssid[0] != '\0';
}

uint32_t app_config_interval_ms(const app_config_t *config) {
    if (config->frequency_tenths == 0) {
        return 0;
    }

    return config->frequency_tenths * 100u;
}
