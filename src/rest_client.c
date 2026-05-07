#include "rest_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"

#define REST_CLIENT_MAX_CONNECTIONS APP_CONFIG_BUTTON_COUNT
#define REST_CLIENT_HOST_MAX 64u
#define REST_CLIENT_PATH_MAX 160u
#define REST_CLIENT_REQUEST_MAX 896u
#define REST_CLIENT_POLL_INTERVAL 10
#define REST_CLIENT_TIMEOUT_POLLS 6

typedef struct {
    char host[REST_CLIENT_HOST_MAX + 1u];
    char path[REST_CLIENT_PATH_MAX + 1u];
    uint16_t port;
} parsed_url_t;

typedef struct {
    bool in_use;
    size_t button_index;
    app_config_button_action_t action;
    parsed_url_t url;
    struct tcp_pcb *pcb;
    char request[REST_CLIENT_REQUEST_MAX];
    size_t request_len;
    size_t bytes_acked;
    size_t response_bytes;
    uint8_t poll_count;
} rest_connection_t;

static rest_connection_t connections[REST_CLIENT_MAX_CONNECTIONS];

static void rest_connection_free(rest_connection_t *connection) {
    if (connection != NULL) {
        memset(connection, 0, sizeof(*connection));
    }
}

static rest_connection_t *rest_connection_alloc(void) {
    for (size_t i = 0; i < REST_CLIENT_MAX_CONNECTIONS; i++) {
        if (!connections[i].in_use) {
            memset(&connections[i], 0, sizeof(connections[i]));
            connections[i].in_use = true;
            return &connections[i];
        }
    }

    return NULL;
}

static bool parse_decimal_port(const char *value, size_t length,
                               uint16_t *port) {
    if (length == 0u) {
        return false;
    }

    uint32_t result = 0;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return false;
        }

        result = (result * 10u) + (uint32_t)(value[i] - '0');
        if (result > 65535u) {
            return false;
        }
    }

    *port = (uint16_t)result;
    return true;
}

static bool rest_parse_url(const char *url, parsed_url_t *parsed) {
    static const char scheme[] = "http://";
    const size_t scheme_len = sizeof(scheme) - 1u;

    if (strncmp(url, scheme, scheme_len) != 0) {
        printf("rest: unsupported URL scheme url='%s'\n", url);
        return false;
    }

    const char *host_start = url + scheme_len;
    const char *path_start = strchr(host_start, '/');
    const char *authority_end =
        path_start == NULL ? url + strlen(url) : path_start;
    const char *port_start = NULL;

    for (const char *cursor = host_start; cursor < authority_end; cursor++) {
        if (*cursor == ':') {
            port_start = cursor + 1;
            authority_end = cursor;
            break;
        }
    }

    const size_t host_len = (size_t)(authority_end - host_start);
    if (host_len == 0u || host_len > REST_CLIENT_HOST_MAX) {
        printf("rest: invalid host length=%lu\n", (unsigned long)host_len);
        return false;
    }

    memset(parsed, 0, sizeof(*parsed));
    memcpy(parsed->host, host_start, host_len);
    parsed->host[host_len] = '\0';
    parsed->port = 80;

    if (port_start != NULL) {
        const char *port_end = path_start == NULL ? url + strlen(url)
                                                  : path_start;
        if (!parse_decimal_port(port_start, (size_t)(port_end - port_start),
                                &parsed->port)) {
            printf("rest: invalid port in url='%s'\n", url);
            return false;
        }
    }

    const char *path = path_start == NULL ? "/" : path_start;
    const size_t path_len = strlen(path);
    if (path_len == 0u || path_len > REST_CLIENT_PATH_MAX) {
        printf("rest: invalid path length=%lu\n", (unsigned long)path_len);
        return false;
    }

    memcpy(parsed->path, path, path_len);
    parsed->path[path_len] = '\0';
    return true;
}

static err_t rest_close(struct tcp_pcb *pcb, rest_connection_t *connection) {
    printf("rest: closing button=%lu pcb=%p response_bytes=%lu\n",
           (unsigned long)connection->button_index, (void *)pcb,
           (unsigned long)connection->response_bytes);

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);

    const err_t err = tcp_close(pcb);
    rest_connection_free(connection);
    if (err != ERR_OK) {
        printf("rest: tcp_close failed err=%d, aborting\n", (int)err);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return ERR_OK;
}

static void rest_error(void *arg, err_t err) {
    rest_connection_t *connection = (rest_connection_t *)arg;
    printf("rest: tcp error button=%lu err=%d\n",
           connection == NULL ? 99ul : (unsigned long)connection->button_index,
           (int)err);
    rest_connection_free(connection);
}

static err_t rest_poll(void *arg, struct tcp_pcb *pcb) {
    rest_connection_t *connection = (rest_connection_t *)arg;
    connection->poll_count++;

    if (connection->poll_count < REST_CLIENT_TIMEOUT_POLLS) {
        return ERR_OK;
    }

    printf("rest: timeout button=%lu pcb=%p\n",
           (unsigned long)connection->button_index, (void *)pcb);
    return rest_close(pcb, connection);
}

static err_t rest_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)pcb;
    rest_connection_t *connection = (rest_connection_t *)arg;
    connection->bytes_acked += len;
    printf("rest: sent ack button=%lu len=%u total=%lu/%lu\n",
           (unsigned long)connection->button_index, (unsigned int)len,
           (unsigned long)connection->bytes_acked,
           (unsigned long)connection->request_len);
    return ERR_OK;
}

static err_t rest_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                       err_t err) {
    rest_connection_t *connection = (rest_connection_t *)arg;

    if (p == NULL) {
        printf("rest: remote closed button=%lu\n",
               (unsigned long)connection->button_index);
        return rest_close(pcb, connection);
    }

    if (err != ERR_OK) {
        printf("rest: recv error button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        pbuf_free(p);
        return rest_close(pcb, connection);
    }

    connection->response_bytes += p->tot_len;
    printf("rest: recv button=%lu len=%u total=%lu\n",
           (unsigned long)connection->button_index, (unsigned int)p->tot_len,
           (unsigned long)connection->response_bytes);

    if (connection->response_bytes == p->tot_len) {
        char preview[96];
        const size_t preview_len =
            pbuf_copy_partial(p, preview, sizeof(preview) - 1u, 0);
        preview[preview_len] = '\0';
        char *line_end = strstr(preview, "\r\n");
        if (line_end != NULL) {
            *line_end = '\0';
        }
        printf("rest: response button=%lu line='%s'\n",
               (unsigned long)connection->button_index, preview);
    }

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static bool rest_build_request(rest_connection_t *connection) {
    const char *method = app_config_action_method_name(connection->action.method);
    const char *body =
        connection->action.method == APP_CONFIG_ACTION_POST
            ? connection->action.body
            : "";
    const size_t body_len = strlen(body);

    int written = 0;
    if (connection->action.method == APP_CONFIG_ACTION_POST) {
        written = snprintf(connection->request, sizeof(connection->request),
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: macropad-pico\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %lu\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "%s",
                           connection->url.path, connection->url.host,
                           (unsigned long)body_len, body);
    } else {
        written = snprintf(connection->request, sizeof(connection->request),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: macropad-pico\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           connection->url.path, connection->url.host);
    }

    if (written < 0 || (size_t)written >= sizeof(connection->request)) {
        printf("rest: request too large button=%lu method=%s url='%s'\n",
               (unsigned long)connection->button_index, method,
               connection->action.url);
        return false;
    }

    connection->request_len = (size_t)written;
    return true;
}

static err_t rest_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    rest_connection_t *connection = (rest_connection_t *)arg;

    if (err != ERR_OK) {
        printf("rest: connect failed button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        rest_connection_free(connection);
        return err;
    }

    printf("rest: connected button=%lu host=%s port=%u\n",
           (unsigned long)connection->button_index, connection->url.host,
           (unsigned int)connection->url.port);

    if (!rest_build_request(connection)) {
        tcp_abort(pcb);
        rest_connection_free(connection);
        return ERR_ABRT;
    }

    tcp_recv(pcb, rest_recv);
    tcp_sent(pcb, rest_sent);
    tcp_poll(pcb, rest_poll, REST_CLIENT_POLL_INTERVAL);
    tcp_err(pcb, rest_error);

    err = tcp_write(pcb, connection->request, connection->request_len,
                    TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("rest: tcp_write failed button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        tcp_abort(pcb);
        rest_connection_free(connection);
        return ERR_ABRT;
    }

    err = tcp_output(pcb);
    if (err != ERR_OK) {
        printf("rest: tcp_output failed button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        tcp_abort(pcb);
        rest_connection_free(connection);
        return ERR_ABRT;
    }

    printf("rest: request sent button=%lu method=%s url='%s' bytes=%lu\n",
           (unsigned long)connection->button_index,
           app_config_action_method_name(connection->action.method),
           connection->action.url, (unsigned long)connection->request_len);
    return ERR_OK;
}

static void rest_connect_to_addr(rest_connection_t *connection,
                                 const ip_addr_t *addr) {
    char ip[16];
    ipaddr_ntoa_r(addr, ip, sizeof(ip));
    printf("rest: connecting button=%lu to %s:%u\n",
           (unsigned long)connection->button_index, ip,
           (unsigned int)connection->url.port);

    connection->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (connection->pcb == NULL) {
        printf("rest: tcp_new failed button=%lu\n",
               (unsigned long)connection->button_index);
        rest_connection_free(connection);
        return;
    }

    tcp_arg(connection->pcb, connection);
    tcp_err(connection->pcb, rest_error);

    const err_t err =
        tcp_connect(connection->pcb, addr, connection->url.port,
                    rest_connected);
    if (err != ERR_OK) {
        printf("rest: tcp_connect failed button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        tcp_abort(connection->pcb);
        rest_connection_free(connection);
    }
}

static void rest_dns_found(const char *name, const ip_addr_t *ipaddr,
                           void *callback_arg) {
    rest_connection_t *connection = (rest_connection_t *)callback_arg;
    if (ipaddr == NULL) {
        printf("rest: dns failed button=%lu host=%s\n",
               (unsigned long)connection->button_index, name);
        rest_connection_free(connection);
        return;
    }

    rest_connect_to_addr(connection, ipaddr);
}

void rest_client_init(void) {
    memset(connections, 0, sizeof(connections));
    printf("rest: client initialized slots=%u\n",
           (unsigned int)REST_CLIENT_MAX_CONNECTIONS);
}

void rest_client_trigger(size_t button_index) {
    if (button_index >= APP_CONFIG_BUTTON_COUNT) {
        printf("rest: invalid button index=%lu\n", (unsigned long)button_index);
        return;
    }

    const app_config_t config = app_config_get();
    const app_config_button_action_t action =
        config.button_actions[button_index];

    if (action.method == APP_CONFIG_ACTION_DISABLED) {
        printf("rest: button=%lu disabled\n", (unsigned long)button_index);
        return;
    }

    rest_connection_t *connection = rest_connection_alloc();
    if (connection == NULL) {
        printf("rest: no free connection slots for button=%lu\n",
               (unsigned long)button_index);
        return;
    }

    connection->button_index = button_index;
    connection->action = action;

    if (!rest_parse_url(action.url, &connection->url)) {
        rest_connection_free(connection);
        return;
    }

    printf("rest: trigger button=%lu method=%s url='%s'\n",
           (unsigned long)button_index,
           app_config_action_method_name(action.method), action.url);

    cyw43_arch_lwip_begin();
    ip_addr_t addr;
    if (ipaddr_aton(connection->url.host, &addr)) {
        rest_connect_to_addr(connection, &addr);
    } else {
        printf("rest: resolving button=%lu host=%s\n",
               (unsigned long)button_index, connection->url.host);
        const err_t err =
            dns_gethostbyname(connection->url.host, &addr, rest_dns_found,
                              connection);
        if (err == ERR_OK) {
            rest_connect_to_addr(connection, &addr);
        } else if (err != ERR_INPROGRESS) {
            printf("rest: dns start failed button=%lu err=%d\n",
                   (unsigned long)button_index, (int)err);
            rest_connection_free(connection);
        }
    }
    cyw43_arch_lwip_end();
}
