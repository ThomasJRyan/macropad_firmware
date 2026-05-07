#include "rest_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "app_config.h"
#include "cyw43.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/ip4.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#define REST_CLIENT_MAX_CONNECTIONS APP_CONFIG_BUTTON_COUNT
#define REST_CLIENT_HOST_MAX 64u
#define REST_CLIENT_PATH_MAX 160u
#define REST_CLIENT_REQUEST_MAX 896u
#define REST_CLIENT_POLL_INTERVAL 10
#define REST_CLIENT_TIMEOUT_POLLS 6
#define REST_CLIENT_CONNECT_TIMEOUT_MS 15000
#define REST_CLIENT_CONNECT_LOG_INTERVAL_MS 1000

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
    bool connected;
    char request[REST_CLIENT_REQUEST_MAX];
    size_t request_len;
    size_t bytes_acked;
    size_t response_bytes;
    uint8_t poll_count;
    absolute_time_t connect_deadline;
    absolute_time_t next_connect_log;
} rest_connection_t;

static rest_connection_t connections[REST_CLIENT_MAX_CONNECTIONS];

static void rest_ip_to_string(const ip_addr_t *addr, char *buffer,
                              size_t buffer_size) {
    if (addr == NULL) {
        snprintf(buffer, buffer_size, "none");
        return;
    }

    ipaddr_ntoa_r(addr, buffer, (int)buffer_size);
}

static void rest_log_netif(const char *label, const struct netif *netif) {
    if (netif == NULL) {
        printf("rest: %s netif=none\n", label);
        return;
    }

    char ip[16];
    char mask[16];
    char gw[16];
    rest_ip_to_string(netif_ip_addr4(netif), ip, sizeof(ip));
    rest_ip_to_string(netif_ip_netmask4(netif), mask, sizeof(mask));
    rest_ip_to_string(netif_ip_gw4(netif), gw, sizeof(gw));

    printf("rest: %s netif=%c%c%u ip=%s mask=%s gw=%s flags=0x%02x\n",
           label, netif->name[0], netif->name[1], (unsigned int)netif->num,
           ip, mask, gw, (unsigned int)netif->flags);
}

static const char *rest_tcp_state_name(enum tcp_state state) {
    switch (state) {
    case CLOSED:
        return "CLOSED";
    case LISTEN:
        return "LISTEN";
    case SYN_SENT:
        return "SYN_SENT";
    case SYN_RCVD:
        return "SYN_RCVD";
    case ESTABLISHED:
        return "ESTABLISHED";
    case FIN_WAIT_1:
        return "FIN_WAIT_1";
    case FIN_WAIT_2:
        return "FIN_WAIT_2";
    case CLOSE_WAIT:
        return "CLOSE_WAIT";
    case CLOSING:
        return "CLOSING";
    case LAST_ACK:
        return "LAST_ACK";
    case TIME_WAIT:
        return "TIME_WAIT";
    default:
        return "UNKNOWN";
    }
}

static void rest_log_pcb(const char *label, const struct tcp_pcb *pcb) {
    if (pcb == NULL) {
        printf("rest: %s pcb=none\n", label);
        return;
    }

    char local_ip[16];
    char remote_ip[16];
    rest_ip_to_string(&pcb->local_ip, local_ip, sizeof(local_ip));
    rest_ip_to_string(&pcb->remote_ip, remote_ip, sizeof(remote_ip));

    printf("rest: %s pcb=%p state=%s local=%s:%u remote=%s:%u nrtx=%u "
           "rtime=%d rto=%d unsent=%p unacked=%p sndq=%u\n",
           label, (const void *)pcb, rest_tcp_state_name(pcb->state),
           local_ip, (unsigned int)pcb->local_port, remote_ip,
           (unsigned int)pcb->remote_port, (unsigned int)pcb->nrtx,
           (int)pcb->rtime, (int)pcb->rto, (void *)pcb->unsent,
           (void *)pcb->unacked, (unsigned int)pcb->snd_queuelen);
}

static void rest_log_arp(const char *label, struct netif *netif,
                         const ip_addr_t *addr) {
    if (netif == NULL || addr == NULL || !IP_IS_V4(addr)) {
        printf("rest: %s arp unavailable\n", label);
        return;
    }

    struct eth_addr *eth = NULL;
    const ip4_addr_t *cached_ip = NULL;
    const ssize_t index =
        etharp_find_addr(netif, ip_2_ip4(addr), &eth, &cached_ip);
    char ip[16];
    rest_ip_to_string(addr, ip, sizeof(ip));

    if (index < 0 || eth == NULL) {
        printf("rest: %s arp miss ip=%s index=%d\n", label, ip,
               (int)index);
        return;
    }

    printf("rest: %s arp hit ip=%s index=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           label, ip, (int)index, eth->addr[0], eth->addr[1], eth->addr[2],
           eth->addr[3], eth->addr[4], eth->addr[5]);
}

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

    connection->connected = true;
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

    struct netif *station_netif = &cyw43_state.netif[CYW43_ITF_STA];
    rest_log_netif("station connect source", station_netif);
    printf("rest: station link_status=%d\n",
           cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));

    struct netif *route = ip4_route(ip_2_ip4(addr));
    rest_log_netif("route before bind", route);
    rest_log_arp("before arp probe", station_netif, addr);
    const err_t arp_err = etharp_query(station_netif, ip_2_ip4(addr), NULL);
    printf("rest: arp probe button=%lu ip=%s err=%d\n",
           (unsigned long)connection->button_index, ip, (int)arp_err);
    rest_log_arp("after arp probe", station_netif, addr);

    connection->pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (connection->pcb == NULL) {
        printf("rest: tcp_new failed button=%lu\n",
               (unsigned long)connection->button_index);
        rest_connection_free(connection);
        return;
    }

    tcp_bind_netif(connection->pcb, station_netif);
    tcp_arg(connection->pcb, connection);
    tcp_err(connection->pcb, rest_error);
    connection->connect_deadline =
        make_timeout_time_ms(REST_CLIENT_CONNECT_TIMEOUT_MS);
    connection->next_connect_log =
        make_timeout_time_ms(REST_CLIENT_CONNECT_LOG_INTERVAL_MS);

    const err_t err =
        tcp_connect(connection->pcb, addr, connection->url.port,
                    rest_connected);
    printf("rest: tcp_connect start button=%lu err=%d pcb=%p\n",
           (unsigned long)connection->button_index, (int)err,
           (void *)connection->pcb);
    if (err != ERR_OK) {
        printf("rest: tcp_connect failed button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        tcp_abort(connection->pcb);
        rest_connection_free(connection);
        return;
    }

    rest_log_pcb("after tcp_connect", connection->pcb);
    const err_t output_err = tcp_output(connection->pcb);
    printf("rest: tcp_output after connect button=%lu err=%d\n",
           (unsigned long)connection->button_index, (int)output_err);
    rest_log_pcb("after tcp_output", connection->pcb);
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

void rest_client_poll(void) {
    cyw43_arch_lwip_begin();
    for (size_t i = 0; i < REST_CLIENT_MAX_CONNECTIONS; i++) {
        rest_connection_t *connection = &connections[i];
        if (!connection->in_use || connection->pcb == NULL) {
            continue;
        }

        if (!connection->connected &&
            time_reached(connection->next_connect_log)) {
            rest_log_pcb("connect pending", connection->pcb);
            rest_log_arp("pending", &cyw43_state.netif[CYW43_ITF_STA],
                         &connection->pcb->remote_ip);
            connection->next_connect_log =
                make_timeout_time_ms(REST_CLIENT_CONNECT_LOG_INTERVAL_MS);
        }

        if (connection->connected ||
            !time_reached(connection->connect_deadline)) {
            continue;
        }

        printf("rest: connect timeout button=%lu host=%s port=%u pcb=%p\n",
               (unsigned long)connection->button_index, connection->url.host,
               (unsigned int)connection->url.port, (void *)connection->pcb);
        rest_log_pcb("connect timeout", connection->pcb);
        rest_log_arp("timeout", &cyw43_state.netif[CYW43_ITF_STA],
                     &connection->pcb->remote_ip);

        tcp_arg(connection->pcb, NULL);
        tcp_recv(connection->pcb, NULL);
        tcp_sent(connection->pcb, NULL);
        tcp_poll(connection->pcb, NULL, 0);
        tcp_err(connection->pcb, NULL);
        tcp_abort(connection->pcb);
        rest_connection_free(connection);
    }
    cyw43_arch_lwip_end();
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
