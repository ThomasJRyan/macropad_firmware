#include "network.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "cyw43.h"
#include "dhserver.h"
#include "lwip/apps/mdns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "pico/critical_section.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define HTTP_PORT 80
#define HTTP_BACKLOG 4
#define HTTP_POLL_INTERVAL 10
#define HTTP_REQUEST_MAX 4096
#define HTTP_RESPONSE_MAX 8192

#define DHCP_SERVER_PORT 67
#define DHCP_LEASE_SECONDS (24 * 60 * 60)
#define DHCP_POOL_SIZE 1

#define WIFI_SCAN_MAX_RESULTS 16
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define NETWORK_PERIODIC_DEBUG 0

#define BUTTON_0_PIN 5u
#define BUTTON_1_PIN 6u

typedef struct {
    bool in_use;
    size_t request_len;
    char request[HTTP_REQUEST_MAX];
    char response[HTTP_RESPONSE_MAX];
} http_connection_t;

typedef struct {
    char ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    uint8_t auth_mode;
    uint16_t channel;
    int16_t rssi;
} wifi_scan_result_t;

static const char button_config_response[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!doctype html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Macropad</title>"
    "<style>"
    "body{font-family:sans-serif;margin:0;background:#f7f7f4;color:#1f2933}"
    "main{max-width:760px;margin:0 auto;padding:24px}"
    "section{margin:18px 0;padding:16px;background:#fff;border:1px solid #ddd}"
    "label{display:block;margin:10px 0 4px;font-weight:600}"
    "input,select,textarea,button{font:inherit;width:100%;box-sizing:border-box}"
    "input,select,textarea{padding:10px;border:1px solid #b8c0cc}"
    "textarea{min-height:90px}"
    "button{margin-top:12px;padding:10px;background:#1f6feb;color:white;border:0}"
    "#status{min-height:1.4em;font-weight:600}"
    "</style>"
    "</head>"
    "<body>"
    "<main>"
    "<h1>Button Actions</h1>"
    "<form id=\"actions\">"
    "<section>"
    "<h2>GP5</h2>"
    "<label>Method</label>"
    "<select name=\"b0_method\">"
    "<option value=\"DISABLED\">Disabled</option>"
    "<option value=\"GET\">GET</option>"
    "<option value=\"POST\">POST</option>"
    "</select>"
    "<label>URL</label>"
    "<input name=\"b0_url\" maxlength=\"128\" "
    "placeholder=\"http://192.168.0.10/action\">"
    "<label>POST body</label>"
    "<textarea name=\"b0_body\" maxlength=\"512\"></textarea>"
    "</section>"
    "<section>"
    "<h2>GP6</h2>"
    "<label>Method</label>"
    "<select name=\"b1_method\">"
    "<option value=\"DISABLED\">Disabled</option>"
    "<option value=\"GET\">GET</option>"
    "<option value=\"POST\">POST</option>"
    "</select>"
    "<label>URL</label>"
    "<input name=\"b1_url\" maxlength=\"128\" "
    "placeholder=\"http://192.168.0.10/action\">"
    "<label>POST body</label>"
    "<textarea name=\"b1_body\" maxlength=\"512\"></textarea>"
    "</section>"
    "<button type=\"submit\">Save Buttons</button>"
    "</form>"
    "<p id=\"status\"></p>"
    "</main>"
    "<script>"
    "const s=document.getElementById('status');"
    "function status(t){s.textContent=t;}"
    "function setAction(i,a){"
    "document.querySelector('[name=b'+i+'_method]').value=a.method;"
    "document.querySelector('[name=b'+i+'_url]').value=a.url||'';"
    "document.querySelector('[name=b'+i+'_body]').value=a.body||'';"
    "}"
    "async function loadConfig(){"
    "const r=await fetch('/api/config');"
    "const c=await r.json();"
    "c.buttons.forEach((a,i)=>setAction(i,a));"
    "}"
    "document.getElementById('actions').addEventListener('submit',async "
    "function(e){"
    "e.preventDefault();"
    "const r=await fetch('/api/config',{method:'POST',headers:{"
    "'Content-Type':'application/x-www-form-urlencoded'},"
    "body:new URLSearchParams(new FormData(this))});"
    "status(r.ok?'Buttons saved':'Button save failed');"
    "if(r.ok)loadConfig();"
    "});"
    "loadConfig();"
    "</script>"
    "</body>"
    "</html>";

static const char wifi_setup_response[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!doctype html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Macropad Wi-Fi Setup</title>"
    "<style>"
    "body{font-family:sans-serif;margin:0;background:#f7f7f4;color:#1f2933}"
    "main{max-width:640px;margin:0 auto;padding:24px}"
    "section{margin:18px 0;padding:16px;background:#fff;border:1px solid #ddd}"
    "label{display:block;margin:10px 0 4px;font-weight:600}"
    "input,select,button{font:inherit;width:100%;box-sizing:border-box}"
    "input,select{padding:10px;border:1px solid #b8c0cc}"
    "button{margin-top:12px;padding:10px;background:#1f6feb;color:white;border:0}"
    "#status{min-height:1.4em;font-weight:600}"
    "</style>"
    "</head>"
    "<body>"
    "<main>"
    "<h1>Wi-Fi Setup</h1>"
    "<section>"
    "<button type=\"button\" id=\"scan\">Scan</button>"
    "<form id=\"wifi\">"
    "<label>Network</label>"
    "<select id=\"networks\" name=\"ssid\"></select>"
    "<label>Password</label>"
    "<input name=\"password\" type=\"password\" maxlength=\"63\">"
    "<button type=\"submit\">Save Wi-Fi</button>"
    "</form>"
    "</section>"
    "<p id=\"status\"></p>"
    "</main>"
    "<script>"
    "const s=document.getElementById('status');"
    "const n=document.getElementById('networks');"
    "function status(t){s.textContent=t;}"
    "async function refreshScan(){"
    "const r=await fetch('/api/wifi/scan');"
    "const j=await r.json();"
    "n.innerHTML='';"
    "j.networks.forEach(function(ap){"
    "const o=document.createElement('option');"
    "o.value=ap.ssid;"
    "o.textContent=ap.ssid+' ('+ap.rssi+' dBm)';"
    "n.appendChild(o);"
    "});"
    "if(j.scanning){status('Scanning...');setTimeout(refreshScan,1000);}"
    "else{status(j.networks.length?'Scan complete':'No networks found');}"
    "}"
    "document.getElementById('scan').addEventListener('click',async "
    "function(){"
    "status('Scanning...');"
    "await fetch('/api/wifi/scan',{method:'POST'});"
    "refreshScan();"
    "});"
    "document.getElementById('wifi').addEventListener('submit',async "
    "function(e){"
    "e.preventDefault();"
    "const r=await fetch('/api/wifi',{method:'POST',headers:{"
    "'Content-Type':'application/x-www-form-urlencoded'},"
    "body:new URLSearchParams(new FormData(this))});"
    "status(r.ok?'Wi-Fi saved':'Wi-Fi save failed');"
    "});"
    "</script>"
    "</body>"
    "</html>";

static const char wifi_saved_response[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"status\":\"saved\"}\n";

static const char bad_request_response[] =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"error\":\"bad_request\"}\n";

static const char scan_error_response[] =
    "HTTP/1.0 500 Internal Server Error\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"error\":\"scan_failed\"}\n";

static const char server_error_response[] =
    "HTTP/1.0 500 Internal Server Error\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"error\":\"save_failed\"}\n";

static const char method_not_allowed_response[] =
    "HTTP/1.0 405 Method Not Allowed\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Allow: GET, POST\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Method Not Allowed\n";

static const char payload_too_large_response[] =
    "HTTP/1.0 413 Payload Too Large\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Payload Too Large\n";

static const char not_found_response[] =
    "HTTP/1.0 404 Not Found\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Not Found\n";

static struct tcp_pcb *http_listener;
static http_connection_t http_connections[HTTP_BACKLOG];
static dhcp_entry_t dhcp_entries[DHCP_POOL_SIZE];
static dhcp_config_t dhcp_config;
static network_mode_t current_mode = NETWORK_MODE_SETUP_AP;
static bool mdns_initialized;
static critical_section_t wifi_scan_lock;
static bool wifi_scan_lock_initialized;
static bool wifi_scan_running;
static wifi_scan_result_t wifi_scan_results[WIFI_SCAN_MAX_RESULTS];
static size_t wifi_scan_count;
static volatile uint32_t http_accept_count;
static volatile uint32_t http_recv_count;
static volatile uint32_t http_response_count;
static volatile err_t last_http_start_err;
static volatile err_t last_mdns_start_err;
static absolute_time_t next_network_debug_at;

static void ip_to_string(const ip_addr_t *addr, char *buffer,
                         size_t buffer_size) {
    if (addr == NULL) {
        snprintf(buffer, buffer_size, "none");
        return;
    }

    ipaddr_ntoa_r(addr, buffer, (int)buffer_size);
}

static void network_log_netif(const char *label, const struct netif *netif) {
    char ip[16];
    char mask[16];
    char gw[16];

    ip_to_string(netif_ip_addr4(netif), ip, sizeof(ip));
    ip_to_string(netif_ip_netmask4(netif), mask, sizeof(mask));
    ip_to_string(netif_ip_gw4(netif), gw, sizeof(gw));

    printf("network: %s netif=%c%c%u ip=%s mask=%s gw=%s flags=0x%02x\n",
           label, netif->name[0], netif->name[1], (unsigned int)netif->num,
           ip, mask, gw, (unsigned int)netif->flags);
}

static size_t http_format(char *response, size_t response_size,
                          const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(response, response_size, format, args);
    va_end(args);

    if (written < 0) {
        response[0] = '\0';
        return 0;
    }

    if ((size_t)written >= response_size) {
        response[response_size - 1] = '\0';
        return response_size - 1;
    }

    return (size_t)written;
}

static bool http_append(char *response, size_t response_size, size_t *length,
                        const char *format, ...) {
    if (*length >= response_size) {
        return false;
    }

    va_list args;
    va_start(args, format);
    const int written =
        vsnprintf(response + *length, response_size - *length, format, args);
    va_end(args);

    if (written < 0) {
        return false;
    }

    if ((size_t)written >= response_size - *length) {
        *length = response_size - 1u;
        response[*length] = '\0';
        return false;
    }

    *length += (size_t)written;
    return true;
}

static bool json_append_string(char *response, size_t response_size,
                               size_t *length, const char *value) {
    if (!http_append(response, response_size, length, "\"")) {
        return false;
    }

    for (size_t i = 0; value[i] != '\0'; i++) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch == '"' || ch == '\\') {
            if (!http_append(response, response_size, length, "\\%c", ch)) {
                return false;
            }
        } else if (ch < 0x20u) {
            if (!http_append(response, response_size, length, "\\u%04x", ch)) {
                return false;
            }
        } else if (!http_append(response, response_size, length, "%c", ch)) {
            return false;
        }
    }

    return http_append(response, response_size, length, "\"");
}

static http_connection_t *http_connection_alloc(void) {
    for (size_t i = 0; i < HTTP_BACKLOG; i++) {
        if (!http_connections[i].in_use) {
            memset(&http_connections[i], 0, sizeof(http_connections[i]));
            http_connections[i].in_use = true;
            return &http_connections[i];
        }
    }

    return NULL;
}

static void http_connection_free(http_connection_t *connection) {
    if (connection != NULL) {
        memset(connection, 0, sizeof(*connection));
    }
}

static err_t http_close(struct tcp_pcb *pcb, http_connection_t *connection) {
    printf("http: closing pcb=%p connection=%p\n", (void *)pcb,
           (void *)connection);
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);
    http_connection_free(connection);

    const err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
        printf("http: tcp_close failed err=%d, aborting pcb=%p\n", (int)err,
               (void *)pcb);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    printf("http: closed pcb=%p\n", (void *)pcb);
    return ERR_OK;
}

static void http_error(void *arg, err_t err) {
    printf("http: tcp error err=%d connection=%p\n", (int)err, arg);
    http_connection_free((http_connection_t *)arg);
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb) {
    printf("http: poll timeout pcb=%p connection=%p\n", (void *)pcb, arg);
    return http_close(pcb, (http_connection_t *)arg);
}

static int ascii_tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }

    return ch;
}

static int http_strncasecmp(const char *left, const char *right, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const int left_ch = ascii_tolower((unsigned char)left[i]);
        const int right_ch = ascii_tolower((unsigned char)right[i]);
        if (left_ch != right_ch || left_ch == '\0') {
            return left_ch - right_ch;
        }
    }

    return 0;
}

static const char *http_header_end(const char *request) {
    return strstr(request, "\r\n\r\n");
}

static size_t http_content_length(const char *request) {
    const char *headers_end = http_header_end(request);
    const char *line = request;

    while (headers_end != NULL && line < headers_end) {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL || line_end > headers_end) {
            line_end = headers_end;
        }

        static const char header_name[] = "Content-Length:";
        const size_t header_name_len = sizeof(header_name) - 1;
        if ((size_t)(line_end - line) > header_name_len &&
            http_strncasecmp(line, header_name, header_name_len) == 0) {
            const char *value = line + header_name_len;
            while (*value == ' ' || *value == '\t') {
                value++;
            }

            size_t length = 0;
            while (*value >= '0' && *value <= '9') {
                length = (length * 10u) + (size_t)(*value - '0');
                value++;
            }
            return length;
        }

        line = line_end + 2;
    }

    return 0;
}

static bool http_request_complete(const http_connection_t *connection) {
    const char *headers_end = http_header_end(connection->request);
    if (headers_end == NULL) {
        return false;
    }

    const size_t header_len =
        (size_t)(headers_end - connection->request) + 4u;
    return connection->request_len >=
           header_len + http_content_length(connection->request);
}

static const char *http_request_body(const char *request) {
    const char *headers_end = http_header_end(request);
    return headers_end == NULL ? "" : headers_end + 4;
}

static const char *http_request_path(const char *request) {
    const char *path = strchr(request, ' ');
    return path == NULL ? NULL : path + 1;
}

static bool http_method_matches(const char *request, const char *method) {
    const size_t method_len = strlen(method);
    return strncmp(request, method, method_len) == 0 &&
           request[method_len] == ' ';
}

static bool http_path_matches(const char *request, const char *path) {
    const char *request_path = http_request_path(request);
    if (request_path == NULL) {
        return false;
    }

    const size_t path_len = strlen(path);
    const char terminator = request_path[path_len];
    return strncmp(request_path, path, path_len) == 0 &&
           (terminator == ' ' || terminator == '?');
}

static bool http_request_matches(const char *request, const char *method,
                                 const char *path) {
    return http_method_matches(request, method) &&
           http_path_matches(request, path);
}

static const char *http_request_query(const char *request) {
    const char *request_path = http_request_path(request);
    if (request_path == NULL) {
        return NULL;
    }

    const char *path_end = strchr(request_path, ' ');
    const char *query = strchr(request_path, '?');
    if (query == NULL || (path_end != NULL && query > path_end)) {
        return NULL;
    }

    return query + 1;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }

    return -1;
}

static bool http_url_decode(const char *source, size_t source_length,
                            char *value, size_t value_size) {
    size_t out = 0;

    for (size_t i = 0; i < source_length; i++) {
        char ch = source[i];
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            if (i + 2u >= source_length) {
                return false;
            }

            const int high = hex_value(source[i + 1u]);
            const int low = hex_value(source[i + 2u]);
            if (high < 0 || low < 0) {
                return false;
            }

            ch = (char)((high << 4) | low);
            i += 2u;
        }

        if (ch == '\0' || out + 1u >= value_size) {
            return false;
        }

        value[out++] = ch;
    }

    value[out] = '\0';
    return true;
}

static bool http_param_value(const char *params, const char *name, char *value,
                             size_t value_size) {
    if (params == NULL) {
        return false;
    }

    const size_t name_len = strlen(name);
    const char *cursor = params;

    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\r' &&
           *cursor != '\n') {
        if (strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            const char *param_value = cursor + name_len + 1;
            size_t length = 0;

            while (param_value[length] != '\0' && param_value[length] != '&' &&
                   param_value[length] != ' ' && param_value[length] != '\r' &&
                   param_value[length] != '\n') {
                length++;
            }

            return http_url_decode(param_value, length, value, value_size);
        }

        cursor = strchr(cursor, '&');
        if (cursor == NULL) {
            return false;
        }
        cursor++;
    }

    return false;
}

static bool http_optional_param_value(const char *body, const char *query,
                                      const char *name, char *value,
                                      size_t value_size) {
    if (http_param_value(body, name, value, value_size) ||
        http_param_value(query, name, value, value_size)) {
        return true;
    }

    value[0] = '\0';
    return true;
}

static bool parse_action_method(const char *value,
                                app_config_action_method_t *method) {
    if (strcmp(value, "DISABLED") == 0 || strcmp(value, "disabled") == 0) {
        *method = APP_CONFIG_ACTION_DISABLED;
        return true;
    }

    if (strcmp(value, "GET") == 0 || strcmp(value, "get") == 0) {
        *method = APP_CONFIG_ACTION_GET;
        return true;
    }

    if (strcmp(value, "POST") == 0 || strcmp(value, "post") == 0) {
        *method = APP_CONFIG_ACTION_POST;
        return true;
    }

    return false;
}

static bool http_parse_action_config(const char *request,
                                     app_config_t *config) {
    const char *body = http_request_body(request);
    const char *query = http_request_query(request);
    app_config_t candidate = *config;

    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        char method_name[16];
        char url_name[16];
        char body_name[16];
        char method_value[16];

        snprintf(method_name, sizeof(method_name), "b%lu_method",
                 (unsigned long)i);
        snprintf(url_name, sizeof(url_name), "b%lu_url", (unsigned long)i);
        snprintf(body_name, sizeof(body_name), "b%lu_body", (unsigned long)i);

        if (!http_param_value(body, method_name, method_value,
                              sizeof(method_value)) &&
            !http_param_value(query, method_name, method_value,
                              sizeof(method_value))) {
            printf("http: missing action method for button=%lu\n",
                   (unsigned long)i);
            return false;
        }

        if (!parse_action_method(method_value,
                                 &candidate.button_actions[i].method)) {
            printf("http: invalid action method='%s' button=%lu\n",
                   method_value, (unsigned long)i);
            return false;
        }

        if (!http_optional_param_value(body, query, url_name,
                                       candidate.button_actions[i].url,
                                       sizeof(candidate.button_actions[i].url)) ||
            !http_optional_param_value(body, query, body_name,
                                       candidate.button_actions[i].body,
                                       sizeof(candidate.button_actions[i].body))) {
            return false;
        }
    }

    if (!app_config_validate(&candidate)) {
        printf("http: action config validation failed\n");
        return false;
    }

    *config = candidate;
    return true;
}

static bool http_parse_wifi_config(const char *request, app_config_t *config) {
    char ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    char password[APP_CONFIG_WIFI_PASSWORD_MAX + 1u];
    const char *body = http_request_body(request);
    const char *query = http_request_query(request);

    const bool has_ssid =
        http_param_value(body, "ssid", ssid, sizeof(ssid)) ||
        http_param_value(query, "ssid", ssid, sizeof(ssid));
    const bool has_password =
        http_param_value(body, "password", password, sizeof(password)) ||
        http_param_value(query, "password", password, sizeof(password));

    if (!has_ssid || !has_password || ssid[0] == '\0') {
        return false;
    }

    app_config_t candidate = *config;
    memset(candidate.wifi_ssid, 0, sizeof(candidate.wifi_ssid));
    memset(candidate.wifi_password, 0, sizeof(candidate.wifi_password));
    memcpy(candidate.wifi_ssid, ssid, strlen(ssid));
    memcpy(candidate.wifi_password, password, strlen(password));

    if (!app_config_validate(&candidate)) {
        return false;
    }

    *config = candidate;
    return true;
}

static size_t http_build_config_response(char *response, size_t response_size,
                                         const app_config_t *config) {
    size_t length = http_format(
        response, response_size,
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"wifi_configured\":%s,\"wifi_ssid\":",
        app_config_has_wifi_credentials(config) ? "true" : "false");

    json_append_string(response, response_size, &length, config->wifi_ssid);
    http_append(response, response_size, &length, ",\"buttons\":[");

    for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
        const app_config_button_action_t *action =
            &config->button_actions[i];
        const unsigned int pin = i == 0 ? BUTTON_0_PIN : BUTTON_1_PIN;

        if (i > 0) {
            http_append(response, response_size, &length, ",");
        }

        http_append(response, response_size, &length,
                    "{\"pin\":%u,\"method\":", pin);
        json_append_string(response, response_size, &length,
                           app_config_action_method_name(action->method));
        http_append(response, response_size, &length, ",\"url\":");
        json_append_string(response, response_size, &length, action->url);
        http_append(response, response_size, &length, ",\"body\":");
        json_append_string(response, response_size, &length, action->body);
        http_append(response, response_size, &length, "}");
    }

    http_append(response, response_size, &length, "]}\n");
    return length;
}

static size_t http_build_index_response(char *response, size_t response_size) {
    const char *page = current_mode == NETWORK_MODE_SETUP_AP
                           ? wifi_setup_response
                           : button_config_response;
    return http_format(response, response_size, "%s", page);
}

static void wifi_scan_state_init(void) {
    if (!wifi_scan_lock_initialized) {
        critical_section_init(&wifi_scan_lock);
        wifi_scan_lock_initialized = true;
    }
}

static int wifi_scan_result_cb(void *env,
                               const cyw43_ev_scan_result_t *result) {
    (void)env;

    if (result->ssid_len == 0 ||
        result->ssid_len > APP_CONFIG_WIFI_SSID_MAX) {
        return 0;
    }

    char ssid[APP_CONFIG_WIFI_SSID_MAX + 1u];
    memcpy(ssid, result->ssid, result->ssid_len);
    ssid[result->ssid_len] = '\0';

    critical_section_enter_blocking(&wifi_scan_lock);

    for (size_t i = 0; i < wifi_scan_count; i++) {
        if (strcmp(wifi_scan_results[i].ssid, ssid) == 0) {
            if (result->rssi > wifi_scan_results[i].rssi) {
                wifi_scan_results[i].auth_mode = result->auth_mode;
                wifi_scan_results[i].channel = result->channel;
                wifi_scan_results[i].rssi = result->rssi;
            }
            critical_section_exit(&wifi_scan_lock);
            return 0;
        }
    }

    if (wifi_scan_count < WIFI_SCAN_MAX_RESULTS) {
        wifi_scan_result_t *slot = &wifi_scan_results[wifi_scan_count++];
        memset(slot, 0, sizeof(*slot));
        memcpy(slot->ssid, ssid, sizeof(slot->ssid));
        slot->auth_mode = result->auth_mode;
        slot->channel = result->channel;
        slot->rssi = result->rssi;
    }

    critical_section_exit(&wifi_scan_lock);
    return 0;
}

static void wifi_scan_refresh_status(void) {
    if (wifi_scan_running && !cyw43_wifi_scan_active(&cyw43_state)) {
        critical_section_enter_blocking(&wifi_scan_lock);
        wifi_scan_running = false;
        critical_section_exit(&wifi_scan_lock);
    }
}

static void wifi_scan_ensure_sta_mode(void) {
    if ((cyw43_state.itf_state & (1u << CYW43_ITF_STA)) == 0) {
        cyw43_arch_enable_sta_mode();
        if (current_mode == NETWORK_MODE_SETUP_AP) {
            cyw43_arch_lwip_begin();
            netif_set_default(&cyw43_state.netif[CYW43_ITF_AP]);
            cyw43_arch_lwip_end();
        }
    }
}

static err_t wifi_scan_start(void) {
    wifi_scan_state_init();
    wifi_scan_refresh_status();

    if (wifi_scan_running) {
        return ERR_OK;
    }

    critical_section_enter_blocking(&wifi_scan_lock);
    memset(wifi_scan_results, 0, sizeof(wifi_scan_results));
    wifi_scan_count = 0;
    wifi_scan_running = true;
    critical_section_exit(&wifi_scan_lock);

    cyw43_wifi_scan_options_t options;
    memset(&options, 0, sizeof(options));
    wifi_scan_ensure_sta_mode();
    const int scan_err =
        cyw43_wifi_scan(&cyw43_state, &options, NULL, wifi_scan_result_cb);
    if (scan_err != 0) {
        critical_section_enter_blocking(&wifi_scan_lock);
        wifi_scan_running = false;
        critical_section_exit(&wifi_scan_lock);
        return ERR_CONN;
    }

    return ERR_OK;
}

static size_t http_build_scan_response(char *response, size_t response_size) {
    wifi_scan_state_init();
    wifi_scan_refresh_status();

    wifi_scan_result_t results[WIFI_SCAN_MAX_RESULTS];
    size_t result_count = 0;
    bool scanning = false;

    critical_section_enter_blocking(&wifi_scan_lock);
    result_count = wifi_scan_count;
    scanning = wifi_scan_running;
    memcpy(results, wifi_scan_results, sizeof(results));
    critical_section_exit(&wifi_scan_lock);

    size_t length = http_format(response, response_size,
                                "HTTP/1.0 200 OK\r\n"
                                "Content-Type: application/json\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "{\"scanning\":%s,\"networks\":[",
                                scanning ? "true" : "false");

    for (size_t i = 0; i < result_count; i++) {
        if (i > 0) {
            http_append(response, response_size, &length, ",");
        }
        http_append(response, response_size, &length, "{\"ssid\":");
        json_append_string(response, response_size, &length, results[i].ssid);
        http_append(response, response_size, &length,
                    ",\"rssi\":%d,\"channel\":%u,\"secure\":%s}",
                    (int)results[i].rssi, (unsigned int)results[i].channel,
                    results[i].auth_mode == CYW43_AUTH_OPEN ? "false"
                                                            : "true");
    }

    http_append(response, response_size, &length, "]}\n");
    return length;
}

static size_t http_copy_response(char *response, size_t response_size,
                                 const char *source) {
    return http_format(response, response_size, "%s", source);
}

static size_t http_build_response(const char *request, char *response,
                                  size_t response_size) {
    if (http_request_matches(request, "GET", "/")) {
        printf("http: route GET /\n");
        return http_build_index_response(response, response_size);
    }

    if (http_request_matches(request, "GET", "/api/config")) {
        printf("http: route GET /api/config\n");
        const app_config_t config = app_config_get();
        return http_build_config_response(response, response_size, &config);
    }

    if (http_request_matches(request, "POST", "/api/config")) {
        printf("http: route POST /api/config\n");
        app_config_t config = app_config_get();
        if (!http_parse_action_config(request, &config)) {
            printf("http: action config parse failed\n");
            return http_copy_response(response, response_size,
                                      bad_request_response);
        }

        if (!app_config_save(&config)) {
            printf("http: action config save failed\n");
            return http_copy_response(response, response_size,
                                      server_error_response);
        }

        for (size_t i = 0; i < APP_CONFIG_BUTTON_COUNT; i++) {
            printf("http: action saved button=%lu method=%s url='%s'\n",
                   (unsigned long)i,
                   app_config_action_method_name(
                       config.button_actions[i].method),
                   config.button_actions[i].url);
        }
        return http_build_config_response(response, response_size, &config);
    }

    if (http_request_matches(request, "GET", "/api/wifi/scan")) {
        printf("http: route GET /api/wifi/scan\n");
        return http_build_scan_response(response, response_size);
    }

    if (http_request_matches(request, "POST", "/api/wifi/scan")) {
        printf("http: route POST /api/wifi/scan\n");
        if (wifi_scan_start() != ERR_OK) {
            printf("http: wifi scan start failed\n");
            return http_copy_response(response, response_size,
                                      scan_error_response);
        }

        return http_build_scan_response(response, response_size);
    }

    if (http_request_matches(request, "POST", "/api/wifi")) {
        printf("http: route POST /api/wifi\n");
        app_config_t config = app_config_get();
        if (!http_parse_wifi_config(request, &config)) {
            printf("http: wifi config parse failed\n");
            return http_copy_response(response, response_size,
                                      bad_request_response);
        }

        if (!app_config_save(&config)) {
            printf("http: wifi config save failed\n");
            return http_copy_response(response, response_size,
                                      server_error_response);
        }

        printf("http: wifi config saved ssid='%s'\n", config.wifi_ssid);
        return http_copy_response(response, response_size, wifi_saved_response);
    }

    if (http_path_matches(request, "/api/config") ||
        http_path_matches(request, "/api/wifi") ||
        http_path_matches(request, "/api/wifi/scan")) {
        printf("http: method not allowed\n");
        return http_copy_response(response, response_size,
                                  method_not_allowed_response);
    }

    printf("http: route not found\n");
    return http_copy_response(response, response_size, not_found_response);
}

static err_t http_write_and_close(struct tcp_pcb *pcb,
                                  http_connection_t *connection,
                                  size_t response_len) {
    printf("http: writing response len=%lu pcb=%p\n",
           (unsigned long)response_len, (void *)pcb);
    err_t err = tcp_write(pcb, connection->response, response_len,
                          TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("http: tcp_write failed err=%d pcb=%p\n", (int)err,
               (void *)pcb);
        http_connection_free(connection);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    err = tcp_output(pcb);
    if (err != ERR_OK) {
        printf("http: tcp_output failed err=%d pcb=%p\n", (int)err,
               (void *)pcb);
        http_connection_free(connection);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    printf("http: tcp_output ok pcb=%p\n", (void *)pcb);
    return http_close(pcb, connection);
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                       err_t err) {
    http_connection_t *connection = (http_connection_t *)arg;

    if (p == NULL) {
        printf("http: recv remote close pcb=%p connection=%p\n", (void *)pcb,
               (void *)connection);
        return http_close(pcb, connection);
    }

    if (err != ERR_OK) {
        printf("http: recv error err=%d len=%u pcb=%p\n", (int)err,
               (unsigned int)p->tot_len, (void *)pcb);
        pbuf_free(p);
        return http_close(pcb, connection);
    }

    tcp_recved(pcb, p->tot_len);
    http_recv_count++;
    printf("http: recv len=%u accumulated=%lu pcb=%p\n",
           (unsigned int)p->tot_len, (unsigned long)connection->request_len,
           (void *)pcb);

    const size_t available =
        sizeof(connection->request) - connection->request_len - 1u;
    if (p->tot_len > available) {
        printf("http: request too large len=%u available=%lu\n",
               (unsigned int)p->tot_len, (unsigned long)available);
        pbuf_free(p);
        const size_t response_len =
            http_copy_response(connection->response,
                               sizeof(connection->response),
                               payload_too_large_response);
        return http_write_and_close(pcb, connection, response_len);
    }

    pbuf_copy_partial(p, connection->request + connection->request_len,
                      p->tot_len, 0);
    connection->request_len += p->tot_len;
    connection->request[connection->request_len] = '\0';
    pbuf_free(p);

    if (!http_request_complete(connection)) {
        printf("http: request incomplete total=%lu\n",
               (unsigned long)connection->request_len);
        return ERR_OK;
    }

    const char *line_end = strstr(connection->request, "\r\n");
    int request_line_len = 0;
    if (line_end != NULL) {
        request_line_len = (int)(line_end - connection->request);
    }
    printf("http: request complete line='%.*s' total=%lu\n",
           request_line_len, connection->request,
           (unsigned long)connection->request_len);

    const size_t response_len =
        http_build_response(connection->request, connection->response,
                            sizeof(connection->response));
    http_response_count++;
    printf("http: responding len=%lu\n", (unsigned long)response_len);
    return http_write_and_close(pcb, connection, response_len);
}

static err_t http_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;

    if (err != ERR_OK || new_pcb == NULL) {
        return ERR_VAL;
    }

    http_accept_count++;
    printf("http: accepted connection count=%lu\n",
           (unsigned long)http_accept_count);

    http_connection_t *connection = http_connection_alloc();
    if (connection == NULL) {
        printf("http: no free connection slots\n");
        tcp_close(new_pcb);
        return ERR_MEM;
    }

    printf("http: connection allocated %p for pcb=%p\n", (void *)connection,
           (void *)new_pcb);
    tcp_arg(new_pcb, connection);
    tcp_recv(new_pcb, http_recv);
    tcp_err(new_pcb, http_error);
    tcp_poll(new_pcb, http_poll, HTTP_POLL_INTERVAL);

    return ERR_OK;
}

static err_t http_server_start(const struct netif *netif) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        printf("http: tcp_new failed\n");
        return ERR_MEM;
    }

    if (netif != NULL) {
        network_log_netif("http target", netif);
    }

    const ip_addr_t *bind_addr = IP_ADDR_ANY;
    char bind_ip[16];
    ip_to_string(bind_addr, bind_ip, sizeof(bind_ip));
    printf("http: binding to %s:%u netif=%p\n", bind_ip, HTTP_PORT,
           (const void *)netif);

    err_t err = tcp_bind(pcb, bind_addr, HTTP_PORT);
    if (err != ERR_OK) {
        printf("http: bind failed err=%d\n", (int)err);
        tcp_close(pcb);
        return err;
    }

    struct tcp_pcb *listener = tcp_listen_with_backlog_and_err(
        pcb, HTTP_BACKLOG, &err);
    if (listener == NULL) {
        printf("http: listen failed err=%d\n", (int)err);
        tcp_close(pcb);
        return err == ERR_OK ? ERR_MEM : err;
    }

    http_listener = listener;
    tcp_accept(http_listener, http_accept);
    printf("http: listening listener=%p\n", (void *)http_listener);

    return ERR_OK;
}

static err_t dhcp_server_start(void) {
    memset(dhcp_entries, 0, sizeof(dhcp_entries));
    IP4_ADDR(&dhcp_entries[0].addr, 172, 16, 4, 2);
    dhcp_entries[0].lease = DHCP_LEASE_SECONDS;

    IP4_ADDR(&dhcp_config.router, 172, 16, 4, 1);
    dhcp_config.port = DHCP_SERVER_PORT;
    dhcp_config.dns.addr = 0;
    dhcp_config.domain = NULL;
    dhcp_config.num_entry = DHCP_POOL_SIZE;
    dhcp_config.entries = dhcp_entries;

    return dhserv_init(&dhcp_config);
}

static void mdns_http_txt(struct mdns_service *service, void *txt_userdata) {
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}

static err_t mdns_server_start(void) {
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];

    if (!mdns_initialized) {
        mdns_resp_init();
        mdns_initialized = true;
    }

    err_t err = mdns_resp_add_netif(netif, MACROPAD_MDNS_HOSTNAME);
    if (err != ERR_OK) {
        printf("mdns: add netif failed err=%d\n", (int)err);
        return err;
    }

    const s8_t service = mdns_resp_add_service(
        netif, "Macropad", "_http", DNSSD_PROTO_TCP, HTTP_PORT,
        mdns_http_txt, NULL);
    (void)service;

    mdns_resp_announce(netif);
    printf("mdns: announced %s.local service=%d\n", MACROPAD_MDNS_HOSTNAME,
           (int)service);
    return ERR_OK;
}

static err_t network_start_setup_ap(void) {
    current_mode = NETWORK_MODE_SETUP_AP;
    printf("network: starting setup AP\n");
    cyw43_arch_enable_ap_mode(MACROPAD_AP_SSID, NULL, CYW43_AUTH_OPEN);

    cyw43_arch_lwip_begin();
    network_log_netif("setup ap", &cyw43_state.netif[CYW43_ITF_AP]);

    err_t err = dhcp_server_start();
    printf("network: dhcp_server_start err=%d\n", (int)err);
    if (err == ERR_OK) {
        err = http_server_start(&cyw43_state.netif[CYW43_ITF_AP]);
    }

    if (err != ERR_OK) {
        printf("network: setup AP start failed err=%d\n", (int)err);
        dhserv_free();
    } else {
        printf("network: setup AP ready\n");
    }

    cyw43_arch_lwip_end();

    return err;
}

static err_t network_start_station(const app_config_t *config) {
    current_mode = NETWORK_MODE_STATION;
    printf("network: connecting to SSID '%s'\n", config->wifi_ssid);
    cyw43_arch_enable_sta_mode();

    const char *password =
        config->wifi_password[0] == '\0' ? NULL : config->wifi_password;
    const int connect_err = cyw43_arch_wifi_connect_timeout_ms(
        config->wifi_ssid, password, CYW43_AUTH_WPA2_MIXED_PSK,
        WIFI_CONNECT_TIMEOUT_MS);
    printf("network: wifi_connect_timeout result=%d\n", connect_err);
    if (connect_err != PICO_OK) {
        printf("network: station connect failed err=%d\n", connect_err);
        cyw43_arch_disable_sta_mode();
        return ERR_CONN;
    }

    cyw43_arch_lwip_begin();

    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    network_log_netif("station connected", netif);
    printf("network: station link_status=%d\n",
           cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));

    err_t err = http_server_start(&cyw43_state.netif[CYW43_ITF_STA]);
    last_http_start_err = err;
    if (err == ERR_OK) {
        last_mdns_start_err = mdns_server_start();
    } else {
        last_mdns_start_err = ERR_ABRT;
    }

    cyw43_arch_lwip_end();

    return err;
}

network_start_result_t network_start(void) {
    wifi_scan_state_init();

    printf("network: lwip MEMP_NUM_SYS_TIMEOUT=%d\n", MEMP_NUM_SYS_TIMEOUT);

    network_start_result_t result = {
        .err = ERR_OK,
        .mode = NETWORK_MODE_SETUP_AP,
        .status = NETWORK_START_SETUP_AP,
    };

    const app_config_t config = app_config_get();
    if (app_config_has_wifi_credentials(&config)) {
        printf("network: stored Wi-Fi credentials found for ssid='%s'\n",
               config.wifi_ssid);
        result.mode = NETWORK_MODE_STATION;
        result.err = network_start_station(&config);
        if (result.err == ERR_OK) {
            printf("network: station startup completed\n");
            result.status = NETWORK_START_STATION_CONNECTED;
            return result;
        }

        if (result.err != ERR_CONN) {
            printf("network: station startup failed hard err=%d\n",
                   (int)result.err);
            result.status = NETWORK_START_STATION_FAILED;
            return result;
        }

        printf("network: station connection failed, falling back to setup AP\n");
        result.mode = NETWORK_MODE_SETUP_AP;
        result.status = NETWORK_START_STATION_FAILED;
    } else {
        printf("network: no stored Wi-Fi credentials, starting setup AP\n");
    }

    result.err = network_start_setup_ap();
    printf("network: setup AP startup completed err=%d\n", (int)result.err);
    return result;
}

void network_debug_poll(void) {
#if NETWORK_PERIODIC_DEBUG
    if (!time_reached(next_network_debug_at)) {
        return;
    }

    next_network_debug_at = make_timeout_time_ms(2000);

    cyw43_arch_lwip_begin();
    const struct netif *netif =
        current_mode == NETWORK_MODE_STATION
            ? &cyw43_state.netif[CYW43_ITF_STA]
            : &cyw43_state.netif[CYW43_ITF_AP];
    char ip[16];
    ip_to_string(netif_ip_addr4(netif), ip, sizeof(ip));
    const int link_status = current_mode == NETWORK_MODE_STATION
                                ? cyw43_tcpip_link_status(&cyw43_state,
                                                          CYW43_ITF_STA)
                                : -1;

    printf("debug: mode=%d ip=%s flags=0x%02x link=%d http=%p start=%d "
           "mdns=%d accept=%lu recv=%lu resp=%lu\n",
           (int)current_mode, ip, (unsigned int)netif->flags, link_status,
           (void *)http_listener, (int)last_http_start_err,
           (int)last_mdns_start_err, (unsigned long)http_accept_count,
           (unsigned long)http_recv_count, (unsigned long)http_response_count);
    cyw43_arch_lwip_end();
#endif
}
