#include "app_config.h"

#include <stddef.h>
#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/flash.h"
#include "pico/critical_section.h"
#include "pico/flash.h"
#include "pico/platform.h"

#define CONFIG_MAGIC 0x4D504346u
#define CONFIG_VERSION 5u
#define CONFIG_VERSION_4 4u
#define CONFIG_VERSION_3 3u
#define CONFIG_VERSION_2 2u
#define CONFIG_VERSION_1 1u
#define CONFIG_FLASH_STORAGE_SIZE (2u * FLASH_SECTOR_SIZE)
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - CONFIG_FLASH_STORAGE_SIZE)
#define CONFIG_LEGACY_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_FLASH_TIMEOUT_MS 1000u
#define CONFIG_LEGACY_BUTTON_COUNT 2u

typedef struct {
    app_config_action_method_t method;
    char url[APP_CONFIG_ACTION_URL_MAX + 1u];
    char body[APP_CONFIG_ACTION_BODY_MAX + 1u];
} app_config_button_action_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    app_config_button_action_t button_actions[APP_CONFIG_BUTTON_COUNT];
    uint32_t checksum;
} config_flash_record_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    app_config_button_action_t button_actions[CONFIG_LEGACY_BUTTON_COUNT];
    uint32_t checksum;
} config_flash_record_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    app_config_button_action_v3_t button_actions[CONFIG_LEGACY_BUTTON_COUNT];
    uint32_t checksum;
} config_flash_record_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t blink_count;
    uint32_t frequency_tenths;
    char wifi_ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    uint32_t checksum;
} config_flash_record_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t blink_count;
    uint32_t frequency_tenths;
    uint32_t checksum;
} config_flash_record_v1_t;

typedef struct {
    uint8_t storage[CONFIG_FLASH_STORAGE_SIZE];
} config_flash_write_t;

static critical_section_t config_lock;
static app_config_t current_config;
static config_flash_write_t pending_write;
extern char __flash_binary_end;

static const uint8_t button_pins[APP_CONFIG_BUTTON_COUNT] = {
    5u, 4u, 3u,
    6u, 7u, 8u,
};

_Static_assert(sizeof(config_flash_record_t) <= CONFIG_FLASH_STORAGE_SIZE,
               "config record must fit in reserved flash storage");

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

static uint32_t config_v4_checksum(const config_flash_record_v4_t *record) {
    return checksum_bytes(record, offsetof(config_flash_record_v4_t, checksum));
}

static uint32_t config_v3_checksum(const config_flash_record_v3_t *record) {
    return checksum_bytes(record, offsetof(config_flash_record_v3_t, checksum));
}

static uint32_t config_v2_checksum(const config_flash_record_v2_t *record) {
    return checksum_bytes(record, offsetof(config_flash_record_v2_t, checksum));
}

static uint32_t config_v1_checksum(const config_flash_record_v1_t *record) {
    return record->magic ^ record->version ^ record->blink_count ^
           record->frequency_tenths ^ 0xA5A55A5Au;
}

static app_config_t config_default(void) {
    app_config_t config;
    memset(&config, 0, sizeof(config));
    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        config.button_actions[i].method = APP_CONFIG_ACTION_DISABLED;
        memcpy(config.button_actions[i].content_type, "application/json",
               sizeof("application/json"));
    }
    return config;
}

static size_t legacy_button_index_to_current(size_t index) {
    return index == 0u ? 0u : 3u;
}

static bool action_method_valid(app_config_action_method_t method) {
    return method == APP_CONFIG_ACTION_DISABLED ||
           method == APP_CONFIG_ACTION_GET ||
           method == APP_CONFIG_ACTION_POST;
}

static bool action_url_valid(const char *url) {
    return strncmp(url, "http://", 7) == 0;
}

static bool action_content_type_valid(const char *value) {
    for (size_t i = 0; value[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch < 0x20u || ch == 0x7fu) {
            return false;
        }
    }

    return true;
}

static bool action_headers_valid(const char *value) {
    bool line_has_content = false;
    bool line_has_colon = false;
    bool line_name_empty = true;

    for (size_t i = 0; value[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)value[i];

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            if (line_has_content && (!line_has_colon || line_name_empty)) {
                return false;
            }
            line_has_content = false;
            line_has_colon = false;
            line_name_empty = true;
            continue;
        }

        if ((ch < 0x20u && ch != '\t') || ch == 0x7fu) {
            return false;
        }

        line_has_content = true;
        if (!line_has_colon && ch == ':') {
            line_has_colon = true;
        } else if (!line_has_colon && ch != ' ' && ch != '\t') {
            line_name_empty = false;
        }
    }

    return !line_has_content || (line_has_colon && !line_name_empty);
}

bool app_config_validate(const app_config_t *config) {
    const size_t ssid_length =
        bounded_string_length(config->wifi_ssid, APP_CONFIG_WIFI_SSID_MAX);
    const size_t password_length = bounded_string_length(
        config->wifi_password, APP_CONFIG_WIFI_PASSWORD_MAX);

    if (ssid_length > APP_CONFIG_WIFI_SSID_MAX ||
        password_length > APP_CONFIG_WIFI_PASSWORD_MAX) {
        return false;
    }

    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        const app_config_button_action_t *action =
            &config->button_actions[i];
        const size_t url_length =
            bounded_string_length(action->url, APP_CONFIG_ACTION_URL_MAX);
        const size_t body_length =
            bounded_string_length(action->body, APP_CONFIG_ACTION_BODY_MAX);
        const size_t content_type_length = bounded_string_length(
            action->content_type, APP_CONFIG_ACTION_CONTENT_TYPE_MAX);
        const size_t headers_length = bounded_string_length(
            action->headers, APP_CONFIG_ACTION_HEADERS_MAX);

        if (!action_method_valid(action->method) ||
            url_length > APP_CONFIG_ACTION_URL_MAX ||
            body_length > APP_CONFIG_ACTION_BODY_MAX ||
            content_type_length > APP_CONFIG_ACTION_CONTENT_TYPE_MAX ||
            headers_length > APP_CONFIG_ACTION_HEADERS_MAX ||
            !action_content_type_valid(action->content_type) ||
            !action_headers_valid(action->headers)) {
            return false;
        }

        if (action->method != APP_CONFIG_ACTION_DISABLED &&
            !action_url_valid(action->url)) {
            return false;
        }
    }

    return true;
}

static bool config_record_valid(const config_flash_record_t *record) {
    app_config_t config = {
        .button_actions = {{0}},
    };
    memcpy(config.wifi_ssid, record->wifi_ssid, sizeof(config.wifi_ssid));
    memcpy(config.wifi_password, record->wifi_password,
           sizeof(config.wifi_password));
    memcpy(config.button_actions, record->button_actions,
           sizeof(config.button_actions));

    return record->magic == CONFIG_MAGIC &&
           record->version == CONFIG_VERSION &&
           record->checksum == config_checksum(record) &&
           app_config_validate(&config);
}

static bool config_record_v4_valid(const config_flash_record_v4_t *record) {
    app_config_t config = config_default();
    memcpy(config.wifi_ssid, record->wifi_ssid, sizeof(config.wifi_ssid));
    memcpy(config.wifi_password, record->wifi_password,
           sizeof(config.wifi_password));

    for (size_t i = 0; i < CONFIG_LEGACY_BUTTON_COUNT; i++) {
        const size_t current_index = legacy_button_index_to_current(i);
        memcpy(&config.button_actions[current_index],
               &record->button_actions[i],
               sizeof(record->button_actions[i]));
    }

    return record->magic == CONFIG_MAGIC &&
           record->version == CONFIG_VERSION_4 &&
           record->checksum == config_v4_checksum(record) &&
           app_config_validate(&config);
}

static bool config_record_v3_valid(const config_flash_record_v3_t *record) {
    app_config_t config = config_default();
    memcpy(config.wifi_ssid, record->wifi_ssid, sizeof(config.wifi_ssid));
    memcpy(config.wifi_password, record->wifi_password,
           sizeof(config.wifi_password));

    for (size_t i = 0; i < CONFIG_LEGACY_BUTTON_COUNT; i++) {
        const size_t current_index = legacy_button_index_to_current(i);
        config.button_actions[current_index].method =
            record->button_actions[i].method;
        memcpy(config.button_actions[current_index].url,
               record->button_actions[i].url,
               sizeof(record->button_actions[i].url));
        memcpy(config.button_actions[current_index].body,
               record->button_actions[i].body,
               sizeof(record->button_actions[i].body));
    }

    return record->magic == CONFIG_MAGIC &&
           record->version == CONFIG_VERSION_3 &&
           record->checksum == config_v3_checksum(record) &&
           app_config_validate(&config);
}

static bool config_record_v2_valid(const config_flash_record_v2_t *record) {
    app_config_t config = config_default();
    memcpy(config.wifi_ssid, record->wifi_ssid, sizeof(config.wifi_ssid));
    memcpy(config.wifi_password, record->wifi_password,
           sizeof(config.wifi_password));

    return record->magic == CONFIG_MAGIC &&
           record->version == CONFIG_VERSION_2 &&
           record->checksum == config_v2_checksum(record) &&
           app_config_validate(&config);
}

static bool config_record_v1_valid(const config_flash_record_v1_t *record) {
    const app_config_t config = config_default();

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
    memcpy(record.wifi_ssid, config->wifi_ssid, sizeof(record.wifi_ssid));
    memcpy(record.wifi_password, config->wifi_password,
           sizeof(record.wifi_password));
    memcpy(record.button_actions, config->button_actions,
           sizeof(record.button_actions));
    record.checksum = config_checksum(&record);
    return record;
}

static void __not_in_flash_func(config_flash_write)(void *param) {
    config_flash_write_t *write = (config_flash_write_t *)param;

    flash_range_erase(CONFIG_FLASH_OFFSET, CONFIG_FLASH_STORAGE_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, write->storage,
                        sizeof(write->storage));
}

void app_config_init(void) {
    critical_section_init(&config_lock);
    hard_assert((uintptr_t)&__flash_binary_end - XIP_BASE <=
                CONFIG_FLASH_OFFSET);

    app_config_t config = config_default();
    const config_flash_record_t *stored =
        (const config_flash_record_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);

    if (config_record_valid(stored)) {
        memcpy(config.wifi_ssid, stored->wifi_ssid,
               sizeof(config.wifi_ssid));
        config.wifi_ssid[APP_CONFIG_WIFI_SSID_MAX] = '\0';
        memcpy(config.wifi_password, stored->wifi_password,
               sizeof(config.wifi_password));
        config.wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX] = '\0';
        memcpy(config.button_actions, stored->button_actions,
               sizeof(config.button_actions));
        for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
            config.button_actions[i].url[APP_CONFIG_ACTION_URL_MAX] = '\0';
            config.button_actions[i].body[APP_CONFIG_ACTION_BODY_MAX] = '\0';
            config.button_actions[i]
                .content_type[APP_CONFIG_ACTION_CONTENT_TYPE_MAX] = '\0';
            config.button_actions[i].headers[APP_CONFIG_ACTION_HEADERS_MAX] =
                '\0';
        }
    } else {
        const uintptr_t legacy_storage = XIP_BASE + CONFIG_LEGACY_FLASH_OFFSET;
        const config_flash_record_v4_t *stored_v4 =
            (const config_flash_record_v4_t *)legacy_storage;
        const config_flash_record_v3_t *stored_v3 =
            (const config_flash_record_v3_t *)legacy_storage;
        if (config_record_v4_valid(stored_v4)) {
            memcpy(config.wifi_ssid, stored_v4->wifi_ssid,
                   sizeof(config.wifi_ssid));
            config.wifi_ssid[APP_CONFIG_WIFI_SSID_MAX] = '\0';
            memcpy(config.wifi_password, stored_v4->wifi_password,
                   sizeof(config.wifi_password));
            config.wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX] = '\0';
            for (size_t i = 0; i < CONFIG_LEGACY_BUTTON_COUNT; i++) {
                const size_t current_index = legacy_button_index_to_current(i);
                memcpy(&config.button_actions[current_index],
                       &stored_v4->button_actions[i],
                       sizeof(stored_v4->button_actions[i]));
                config.button_actions[current_index]
                    .url[APP_CONFIG_ACTION_URL_MAX] = '\0';
                config.button_actions[current_index]
                    .body[APP_CONFIG_ACTION_BODY_MAX] = '\0';
                config.button_actions[current_index]
                    .content_type[APP_CONFIG_ACTION_CONTENT_TYPE_MAX] = '\0';
                config.button_actions[current_index]
                    .headers[APP_CONFIG_ACTION_HEADERS_MAX] = '\0';
            }
        } else if (config_record_v3_valid(stored_v3)) {
            memcpy(config.wifi_ssid, stored_v3->wifi_ssid,
                   sizeof(config.wifi_ssid));
            config.wifi_ssid[APP_CONFIG_WIFI_SSID_MAX] = '\0';
            memcpy(config.wifi_password, stored_v3->wifi_password,
                   sizeof(config.wifi_password));
            config.wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX] = '\0';
            for (size_t i = 0; i < CONFIG_LEGACY_BUTTON_COUNT; i++) {
                const size_t current_index = legacy_button_index_to_current(i);
                config.button_actions[current_index].method =
                    stored_v3->button_actions[i].method;
                memcpy(config.button_actions[current_index].url,
                       stored_v3->button_actions[i].url,
                       sizeof(stored_v3->button_actions[i].url));
                config.button_actions[current_index]
                    .url[APP_CONFIG_ACTION_URL_MAX] = '\0';
                memcpy(config.button_actions[current_index].body,
                       stored_v3->button_actions[i].body,
                       sizeof(stored_v3->button_actions[i].body));
                config.button_actions[current_index]
                    .body[APP_CONFIG_ACTION_BODY_MAX] = '\0';
            }
        } else {
            const config_flash_record_v2_t *stored_v2 =
                (const config_flash_record_v2_t *)legacy_storage;
            if (config_record_v2_valid(stored_v2)) {
                memcpy(config.wifi_ssid, stored_v2->wifi_ssid,
                       sizeof(config.wifi_ssid));
                config.wifi_ssid[APP_CONFIG_WIFI_SSID_MAX] = '\0';
                memcpy(config.wifi_password, stored_v2->wifi_password,
                       sizeof(config.wifi_password));
                config.wifi_password[APP_CONFIG_WIFI_PASSWORD_MAX] = '\0';
            } else {
                const config_flash_record_v1_t *stored_v1 =
                    (const config_flash_record_v1_t *)legacy_storage;
                if (config_record_v1_valid(stored_v1)) {
                    config = config_default();
                }
            }
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

    memset(&pending_write, 0xff, sizeof(pending_write));

    const config_flash_record_t record = config_record_from_app(config);
    memcpy(pending_write.storage, &record, sizeof(record));

    if (flash_safe_execute(config_flash_write, &pending_write,
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

const char *app_config_action_method_name(app_config_action_method_t method) {
    switch (method) {
    case APP_CONFIG_ACTION_GET:
        return "GET";
    case APP_CONFIG_ACTION_POST:
        return "POST";
    case APP_CONFIG_ACTION_DISABLED:
    default:
        return "DISABLED";
    }
}

uint8_t app_config_button_pin(size_t button_index) {
    if (button_index >= APP_CONFIG_BUTTON_COUNT) {
        return 0u;
    }

    return button_pins[button_index];
}
