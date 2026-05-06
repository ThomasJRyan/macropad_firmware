#include "network.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "blink_request.h"
#include "cyw43.h"
#include "dhserver.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"

#define HTTP_PORT 80
#define HTTP_BACKLOG 4
#define HTTP_POLL_INTERVAL 10
#define HTTP_REQUEST_MAX 768
#define HTTP_RESPONSE_MAX 1536

#define DHCP_SERVER_PORT 67
#define DHCP_LEASE_SECONDS (24 * 60 * 60)
#define DHCP_POOL_SIZE 1

typedef struct {
    bool in_use;
    size_t request_len;
    char request[HTTP_REQUEST_MAX];
    char response[HTTP_RESPONSE_MAX];
} http_connection_t;

static const char index_response_template[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "<!doctype html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Macropad Setup</title>"
    "</head>"
    "<body>"
    "<h1>Hello World</h1>"
    "<form id=\"config\">"
    "<label>Blinks "
    "<input name=\"blinks\" type=\"number\" min=\"0\" max=\"100\" step=\"1\" "
    "value=\"%lu\">"
    "</label>"
    "<label> Frequency "
    "<input name=\"frequency\" type=\"number\" min=\"0\" max=\"60\" "
    "step=\"0.1\" value=\"%lu.%lu\">"
    "</label>"
    "<button type=\"submit\">Save</button>"
    "</form>"
    "<button type=\"button\" id=\"blink\">Blink LED</button>"
    "<p id=\"status\"></p>"
    "<script>"
    "const s=document.getElementById('status');"
    "document.getElementById('config').addEventListener('submit',async "
    "function(e){"
    "e.preventDefault();"
    "const r=await fetch('/api/config',{method:'POST',headers:{"
    "'Content-Type':'application/x-www-form-urlencoded'},"
    "body:new URLSearchParams(new FormData(this))});"
    "s.textContent=r.ok?'Saved':'Save failed';"
    "});"
    "document.getElementById('blink').addEventListener('click',async "
    "function(){"
    "await fetch('/api/blink',{method:'POST'});"
    "});"
    "</script>"
    "</body>"
    "</html>";

static const char blink_response[] =
    "HTTP/1.0 202 Accepted\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"status\":\"queued\"}\n";

static const char bad_request_response[] =
    "HTTP/1.0 400 Bad Request\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"error\":\"invalid_config\"}\n";

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
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);
    http_connection_free(connection);

    const err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return ERR_OK;
}

static void http_error(void *arg, err_t err) {
    (void)err;
    http_connection_free((http_connection_t *)arg);
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb) {
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

            if (length >= value_size) {
                return false;
            }

            memcpy(value, param_value, length);
            value[length] = '\0';
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor == NULL) {
            return false;
        }
        cursor++;
    }

    return false;
}

static bool parse_uint32_value(const char *value, uint32_t *out) {
    if (*value == '\0') {
        return false;
    }

    uint32_t result = 0;
    while (*value >= '0' && *value <= '9') {
        result = (result * 10u) + (uint32_t)(*value - '0');
        value++;
    }

    if (*value != '\0') {
        return false;
    }

    *out = result;
    return true;
}

static bool parse_frequency_tenths(const char *value, uint32_t *out) {
    if (*value == '\0') {
        return false;
    }

    uint32_t whole = 0;
    while (*value >= '0' && *value <= '9') {
        whole = (whole * 10u) + (uint32_t)(*value - '0');
        value++;
    }

    uint32_t tenths = 0;
    if (*value == '.') {
        value++;
        if (*value >= '0' && *value <= '9') {
            tenths = (uint32_t)(*value - '0');
            value++;
        }

        while (*value == '0') {
            value++;
        }
    }

    if (*value != '\0') {
        return false;
    }

    *out = (whole * 10u) + tenths;
    return true;
}

static bool http_parse_config(const char *request, app_config_t *config) {
    char blinks_value[12];
    char frequency_value[12];
    const char *body = http_request_body(request);
    const char *query = http_request_query(request);

    const bool has_blinks =
        http_param_value(body, "blinks", blinks_value, sizeof(blinks_value)) ||
        http_param_value(query, "blinks", blinks_value, sizeof(blinks_value));
    const bool has_frequency =
        http_param_value(body, "frequency", frequency_value,
                         sizeof(frequency_value)) ||
        http_param_value(query, "frequency", frequency_value,
                         sizeof(frequency_value));

    if (!has_blinks || !has_frequency) {
        return false;
    }

    return parse_uint32_value(blinks_value, &config->blink_count) &&
           parse_frequency_tenths(frequency_value,
                                  &config->frequency_tenths) &&
           app_config_validate(config);
}

static size_t http_build_config_response(char *response, size_t response_size,
                                         const app_config_t *config) {
    return http_format(response, response_size,
                       "HTTP/1.0 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "{\"blinks\":%lu,\"frequency\":%lu.%lu}\n",
                       (unsigned long)config->blink_count,
                       (unsigned long)(config->frequency_tenths / 10u),
                       (unsigned long)(config->frequency_tenths % 10u));
}

static size_t http_build_index_response(char *response, size_t response_size) {
    const app_config_t config = app_config_get();

    return http_format(response, response_size, index_response_template,
                       (unsigned long)config.blink_count,
                       (unsigned long)(config.frequency_tenths / 10u),
                       (unsigned long)(config.frequency_tenths % 10u));
}

static size_t http_copy_response(char *response, size_t response_size,
                                 const char *source) {
    return http_format(response, response_size, "%s", source);
}

static size_t http_build_response(const char *request, char *response,
                                  size_t response_size) {
    if (http_request_matches(request, "GET", "/")) {
        return http_build_index_response(response, response_size);
    }

    if (http_request_matches(request, "GET", "/api/config")) {
        const app_config_t config = app_config_get();
        return http_build_config_response(response, response_size, &config);
    }

    if (http_request_matches(request, "POST", "/api/config")) {
        app_config_t config;
        if (!http_parse_config(request, &config)) {
            return http_copy_response(response, response_size,
                                      bad_request_response);
        }

        if (!app_config_save(&config)) {
            return http_copy_response(response, response_size,
                                      server_error_response);
        }

        return http_build_config_response(response, response_size, &config);
    }

    if (http_request_matches(request, "POST", "/api/blink")) {
        blink_request_enqueue_web();
        return http_copy_response(response, response_size, blink_response);
    }

    if (http_path_matches(request, "/api/config") ||
        http_path_matches(request, "/api/blink")) {
        return http_copy_response(response, response_size,
                                  method_not_allowed_response);
    }

    return http_copy_response(response, response_size, not_found_response);
}

static err_t http_write_and_close(struct tcp_pcb *pcb,
                                  http_connection_t *connection,
                                  size_t response_len) {
    err_t err = tcp_write(pcb, connection->response, response_len,
                          TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        http_connection_free(connection);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    err = tcp_output(pcb);
    if (err != ERR_OK) {
        http_connection_free(connection);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return http_close(pcb, connection);
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                       err_t err) {
    http_connection_t *connection = (http_connection_t *)arg;

    if (p == NULL) {
        return http_close(pcb, connection);
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return http_close(pcb, connection);
    }

    tcp_recved(pcb, p->tot_len);

    const size_t available =
        sizeof(connection->request) - connection->request_len - 1u;
    if (p->tot_len > available) {
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
        return ERR_OK;
    }

    const size_t response_len =
        http_build_response(connection->request, connection->response,
                            sizeof(connection->response));
    return http_write_and_close(pcb, connection, response_len);
}

static err_t http_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;

    if (err != ERR_OK || new_pcb == NULL) {
        return ERR_VAL;
    }

    http_connection_t *connection = http_connection_alloc();
    if (connection == NULL) {
        tcp_close(new_pcb);
        return ERR_MEM;
    }

    tcp_arg(new_pcb, connection);
    tcp_recv(new_pcb, http_recv);
    tcp_err(new_pcb, http_error);
    tcp_poll(new_pcb, http_poll, HTTP_POLL_INTERVAL);

    return ERR_OK;
}

static err_t http_server_start(void) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        return ERR_MEM;
    }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT);
    if (err != ERR_OK) {
        tcp_close(pcb);
        return err;
    }

    struct tcp_pcb *listener = tcp_listen_with_backlog_and_err(
        pcb, HTTP_BACKLOG, &err);
    if (listener == NULL) {
        tcp_close(pcb);
        return err == ERR_OK ? ERR_MEM : err;
    }

    http_listener = listener;
    tcp_accept(http_listener, http_accept);

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

err_t network_start(void) {
    cyw43_arch_enable_ap_mode(MACROPAD_AP_SSID, NULL, CYW43_AUTH_OPEN);

    cyw43_arch_lwip_begin();

    err_t err = dhcp_server_start();
    if (err == ERR_OK) {
        err = http_server_start();
    }

    if (err != ERR_OK) {
        dhserv_free();
    }

    cyw43_arch_lwip_end();

    return err;
}
