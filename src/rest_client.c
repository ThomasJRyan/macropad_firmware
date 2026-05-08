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
#include "lwip/pbuf.h"
#include "lwip/prot/dns.h"
#include "lwip/prot/iana.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
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
#define REST_CLIENT_MDNS_PACKET_MAX 768u
#define REST_CLIENT_MDNS_QUERY_MAX 256u
#define REST_CLIENT_MDNS_TIMEOUT_MS 5000
#define REST_CLIENT_MDNS_RETRY_MS 1000
#define REST_CLIENT_MDNS_MULTICAST_A 224
#define REST_CLIENT_MDNS_MULTICAST_B 0
#define REST_CLIENT_MDNS_MULTICAST_C 0
#define REST_CLIENT_MDNS_MULTICAST_D 251
#define REST_DNS_CLASS_QU 0x8000u
#define REST_DNS_CLASS_MASK 0x7fffu

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
    struct udp_pcb *mdns_pcb;
    struct tcp_pcb *pcb;
    bool connected;
    char request[REST_CLIENT_REQUEST_MAX];
    size_t request_len;
    size_t bytes_acked;
    size_t response_bytes;
    uint8_t poll_count;
    uint8_t mdns_query_count;
    absolute_time_t mdns_deadline;
    absolute_time_t next_mdns_query;
    absolute_time_t connect_deadline;
    absolute_time_t next_connect_log;
} rest_connection_t;

static rest_connection_t connections[REST_CLIENT_MAX_CONNECTIONS];

static void rest_connect_to_addr(rest_connection_t *connection,
                                 const ip_addr_t *addr);

static int rest_ascii_tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }

    return ch;
}

static int rest_strcasecmp(const char *left, const char *right) {
    while (*left != '\0' || *right != '\0') {
        const int left_ch = rest_ascii_tolower((unsigned char)*left);
        const int right_ch = rest_ascii_tolower((unsigned char)*right);
        if (left_ch != right_ch) {
            return left_ch - right_ch;
        }

        if (*left != '\0') {
            left++;
        }
        if (*right != '\0') {
            right++;
        }
    }

    return 0;
}

static bool rest_host_is_mdns(const char *host) {
    const size_t length = strlen(host);
    static const char suffix[] = ".local";
    const size_t suffix_length = sizeof(suffix) - 1u;

    return length > suffix_length &&
           rest_strcasecmp(host + length - suffix_length, suffix) == 0;
}

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

static void rest_mdns_stop(rest_connection_t *connection) {
    if (connection->mdns_pcb == NULL) {
        return;
    }

    udp_recv(connection->mdns_pcb, NULL, NULL);
    udp_remove(connection->mdns_pcb);
    connection->mdns_pcb = NULL;
}

static void rest_connection_free(rest_connection_t *connection) {
    if (connection != NULL) {
        rest_mdns_stop(connection);
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

static uint16_t rest_dns_read_u16(const uint8_t *packet, size_t offset) {
    return (uint16_t)(((uint16_t)packet[offset] << 8u) |
                      (uint16_t)packet[offset + 1u]);
}

static uint32_t rest_dns_read_u32(const uint8_t *packet, size_t offset) {
    return ((uint32_t)packet[offset] << 24u) |
           ((uint32_t)packet[offset + 1u] << 16u) |
           ((uint32_t)packet[offset + 2u] << 8u) |
           (uint32_t)packet[offset + 3u];
}

static bool rest_dns_decode_name(const uint8_t *packet, size_t packet_len,
                                 size_t offset, char *name,
                                 size_t name_size, size_t *next_offset) {
    size_t pos = offset;
    size_t out = 0;
    bool jumped = false;
    unsigned int jumps = 0;

    if (name_size == 0u) {
        return false;
    }

    while (true) {
        if (pos >= packet_len) {
            return false;
        }

        const uint8_t label_len = packet[pos];
        if (label_len == 0u) {
            if (!jumped) {
                *next_offset = pos + 1u;
            }
            name[out] = '\0';
            return true;
        }

        if ((label_len & 0xc0u) == 0xc0u) {
            if (pos + 1u >= packet_len) {
                return false;
            }

            const size_t pointer =
                (((size_t)label_len & 0x3fu) << 8u) | packet[pos + 1u];
            if (pointer >= packet_len || jumps++ > 8u) {
                return false;
            }

            if (!jumped) {
                *next_offset = pos + 2u;
                jumped = true;
            }
            pos = pointer;
            continue;
        }

        if ((label_len & 0xc0u) != 0u) {
            return false;
        }

        pos++;
        if (pos + label_len > packet_len) {
            return false;
        }

        if (out > 0u) {
            if (out + 1u >= name_size) {
                return false;
            }
            name[out++] = '.';
        }

        if (out + label_len >= name_size) {
            return false;
        }
        for (size_t i = 0; i < label_len; i++) {
            name[out++] = (char)rest_ascii_tolower(packet[pos + i]);
        }

        pos += label_len;
    }
}

static bool rest_dns_write_name(uint8_t *packet, size_t packet_size,
                                size_t *offset, const char *name) {
    const char *cursor = name;

    while (*cursor != '\0') {
        const char *dot = strchr(cursor, '.');
        const size_t label_len =
            dot == NULL ? strlen(cursor) : (size_t)(dot - cursor);
        if (label_len == 0u || label_len > 63u ||
            *offset + label_len + 1u >= packet_size) {
            return false;
        }

        packet[(*offset)++] = (uint8_t)label_len;
        memcpy(packet + *offset, cursor, label_len);
        *offset += label_len;

        if (dot == NULL) {
            break;
        }
        cursor = dot + 1;
    }

    if (*offset >= packet_size) {
        return false;
    }

    packet[(*offset)++] = 0;
    return true;
}

static bool rest_mdns_parse_response(const uint8_t *packet, size_t packet_len,
                                     const char *host, ip_addr_t *addr) {
    if (packet_len < SIZEOF_DNS_HDR) {
        return false;
    }

    const uint16_t question_count = rest_dns_read_u16(packet, 4);
    const uint16_t answer_count = rest_dns_read_u16(packet, 6);
    const uint16_t authority_count = rest_dns_read_u16(packet, 8);
    const uint16_t additional_count = rest_dns_read_u16(packet, 10);
    const uint32_t record_count = (uint32_t)answer_count + authority_count +
                                  additional_count;
    size_t offset = SIZEOF_DNS_HDR;

    for (uint16_t i = 0; i < question_count; i++) {
        char name[REST_CLIENT_HOST_MAX + 1u];
        size_t next = 0;
        if (!rest_dns_decode_name(packet, packet_len, offset, name,
                                  sizeof(name), &next) ||
            next + 4u > packet_len) {
            return false;
        }
        offset = next + 4u;
    }

    for (uint32_t i = 0; i < record_count; i++) {
        char record_name[REST_CLIENT_HOST_MAX + 1u];
        size_t next = 0;
        if (!rest_dns_decode_name(packet, packet_len, offset, record_name,
                                  sizeof(record_name), &next) ||
            next + 10u > packet_len) {
            return false;
        }

        const uint16_t type = rest_dns_read_u16(packet, next);
        const uint16_t record_class =
            rest_dns_read_u16(packet, next + 2u) & REST_DNS_CLASS_MASK;
        (void)rest_dns_read_u32(packet, next + 4u);
        const uint16_t rdlength = rest_dns_read_u16(packet, next + 8u);
        const size_t rdata = next + 10u;
        if (rdata + rdlength > packet_len) {
            return false;
        }

        if (type == DNS_RRTYPE_A && record_class == DNS_RRCLASS_IN &&
            rdlength == 4u && rest_strcasecmp(record_name, host) == 0) {
            IP_ADDR4(addr, packet[rdata], packet[rdata + 1u],
                     packet[rdata + 2u], packet[rdata + 3u]);
            return true;
        }

        offset = rdata + rdlength;
    }

    return false;
}

static err_t rest_mdns_send_query(rest_connection_t *connection) {
    uint8_t query[REST_CLIENT_MDNS_QUERY_MAX];
    memset(query, 0, sizeof(query));
    query[5] = 1u;

    size_t offset = SIZEOF_DNS_HDR;
    if (!rest_dns_write_name(query, sizeof(query), &offset,
                             connection->url.host) ||
        offset + 4u > sizeof(query)) {
        printf("rest: mdns query build failed host=%s\n",
               connection->url.host);
        return ERR_VAL;
    }

    query[offset++] = 0;
    query[offset++] = DNS_RRTYPE_A;
    const uint16_t question_class = REST_DNS_CLASS_QU | DNS_RRCLASS_IN;
    query[offset++] = (uint8_t)(question_class >> 8u);
    query[offset++] = (uint8_t)(question_class & 0xffu);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)offset, PBUF_RAM);
    if (p == NULL) {
        return ERR_MEM;
    }

    err_t err = pbuf_take(p, query, (u16_t)offset);
    if (err == ERR_OK) {
        ip_addr_t multicast_addr;
        IP_ADDR4(&multicast_addr, REST_CLIENT_MDNS_MULTICAST_A,
                 REST_CLIENT_MDNS_MULTICAST_B, REST_CLIENT_MDNS_MULTICAST_C,
                 REST_CLIENT_MDNS_MULTICAST_D);
        err = udp_sendto_if(connection->mdns_pcb, p, &multicast_addr,
                            LWIP_IANA_PORT_MDNS,
                            &cyw43_state.netif[CYW43_ITF_STA]);
    }
    pbuf_free(p);

    printf("rest: mdns query button=%lu host=%s err=%d attempt=%u\n",
           (unsigned long)connection->button_index, connection->url.host,
           (int)err, (unsigned int)(connection->mdns_query_count + 1u));
    if (err == ERR_OK) {
        connection->mdns_query_count++;
    }
    return err;
}

static void rest_mdns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *addr, u16_t port) {
    (void)pcb;
    rest_connection_t *connection = (rest_connection_t *)arg;
    uint8_t packet[REST_CLIENT_MDNS_PACKET_MAX];
    const size_t packet_len =
        p->tot_len > sizeof(packet) ? sizeof(packet) : p->tot_len;
    pbuf_copy_partial(p, packet, (u16_t)packet_len, 0);
    pbuf_free(p);

    ip_addr_t resolved_addr;
    if (!rest_mdns_parse_response(packet, packet_len, connection->url.host,
                                  &resolved_addr)) {
        char source_ip[16];
        rest_ip_to_string(addr, source_ip, sizeof(source_ip));
        printf("rest: mdns response ignored host=%s from=%s:%u len=%lu\n",
               connection->url.host, source_ip, (unsigned int)port,
               (unsigned long)packet_len);
        return;
    }

    char resolved_ip[16];
    rest_ip_to_string(&resolved_addr, resolved_ip, sizeof(resolved_ip));
    printf("rest: mdns resolved button=%lu host=%s ip=%s\n",
           (unsigned long)connection->button_index, connection->url.host,
           resolved_ip);

    rest_mdns_stop(connection);
    rest_connect_to_addr(connection, &resolved_addr);
}

static err_t rest_mdns_start(rest_connection_t *connection) {
    connection->mdns_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (connection->mdns_pcb == NULL) {
        printf("rest: mdns udp_new failed button=%lu\n",
               (unsigned long)connection->button_index);
        return ERR_MEM;
    }

#if LWIP_MULTICAST_TX_OPTIONS
    udp_set_multicast_ttl(connection->mdns_pcb, 255);
#else
    connection->mdns_pcb->ttl = 255;
#endif
    udp_bind_netif(connection->mdns_pcb, &cyw43_state.netif[CYW43_ITF_STA]);
    err_t err = udp_bind(connection->mdns_pcb, IP_ADDR_ANY, 0);
    if (err != ERR_OK) {
        printf("rest: mdns udp_bind failed button=%lu err=%d\n",
               (unsigned long)connection->button_index, (int)err);
        rest_mdns_stop(connection);
        return err;
    }

    udp_recv(connection->mdns_pcb, rest_mdns_recv, connection);
    connection->mdns_deadline =
        make_timeout_time_ms(REST_CLIENT_MDNS_TIMEOUT_MS);
    connection->next_mdns_query =
        make_timeout_time_ms(REST_CLIENT_MDNS_RETRY_MS);

    printf("rest: mdns resolving button=%lu host=%s local_port=%u\n",
           (unsigned long)connection->button_index, connection->url.host,
           (unsigned int)connection->mdns_pcb->local_port);
    err = rest_mdns_send_query(connection);
    if (err != ERR_OK) {
        rest_mdns_stop(connection);
    }
    return err;
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
        if (connection->in_use && connection->mdns_pcb != NULL) {
            if (time_reached(connection->mdns_deadline)) {
                printf("rest: mdns timeout button=%lu host=%s attempts=%u\n",
                       (unsigned long)connection->button_index,
                       connection->url.host,
                       (unsigned int)connection->mdns_query_count);
                rest_connection_free(connection);
                continue;
            }

            if (time_reached(connection->next_mdns_query)) {
                const err_t err = rest_mdns_send_query(connection);
                connection->next_mdns_query =
                    make_timeout_time_ms(REST_CLIENT_MDNS_RETRY_MS);
                if (err != ERR_OK) {
                    rest_connection_free(connection);
                }
            }
            continue;
        }

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
    } else if (rest_host_is_mdns(connection->url.host)) {
        printf("rest: resolving button=%lu host=%s via mdns\n",
               (unsigned long)button_index, connection->url.host);
        if (rest_mdns_start(connection) != ERR_OK) {
            rest_connection_free(connection);
        }
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
