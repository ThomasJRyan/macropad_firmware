#include "network.h"

#include <string.h>

#include "blink_request.h"
#include "cyw43.h"
#include "dhserver.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"

#define HTTP_PORT 80
#define HTTP_BACKLOG 4
#define HTTP_POLL_INTERVAL 10
#define HTTP_REQUEST_MAX 256

#define DHCP_SERVER_PORT 67
#define DHCP_LEASE_SECONDS (24 * 60 * 60)
#define DHCP_POOL_SIZE 1

static const char index_response[] =
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
    "<button type=\"button\" id=\"blink\">Blink LED</button>"
    "<script>"
    "document.getElementById('blink').addEventListener('click',function(){"
    "fetch('/api/blink',{method:'POST'});"
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

static const char method_not_allowed_response[] =
    "HTTP/1.0 405 Method Not Allowed\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Allow: POST\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Method Not Allowed\n";

static const char not_found_response[] =
    "HTTP/1.0 404 Not Found\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Not Found\n";

static struct tcp_pcb *http_listener;
static dhcp_entry_t dhcp_entries[DHCP_POOL_SIZE];
static dhcp_config_t dhcp_config;

static err_t http_close(struct tcp_pcb *pcb) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);

    const err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return ERR_OK;
}

static void http_error(void *arg, err_t err) {
    (void)arg;
    (void)err;
}

static err_t http_poll(void *arg, struct tcp_pcb *pcb) {
    (void)arg;
    return http_close(pcb);
}

static bool http_request_matches(const char *request, const char *method,
                                 const char *path) {
    const size_t method_len = strlen(method);
    const size_t path_len = strlen(path);

    if (strncmp(request, method, method_len) != 0) {
        return false;
    }

    const char *request_path = request + method_len;
    if (*request_path != ' ') {
        return false;
    }
    request_path++;

    return strncmp(request_path, path, path_len) == 0 &&
           request_path[path_len] == ' ';
}

static bool http_path_matches(const char *request, const char *path) {
    const char *request_path = strchr(request, ' ');
    if (request_path == NULL) {
        return false;
    }
    request_path++;

    const size_t path_len = strlen(path);
    return strncmp(request_path, path, path_len) == 0 &&
           request_path[path_len] == ' ';
}

static const char *http_response_for_request(const char *request) {
    if (http_request_matches(request, "GET", "/")) {
        return index_response;
    }

    if (http_request_matches(request, "POST", "/api/blink")) {
        blink_request_enqueue_web();
        return blink_response;
    }

    if (http_path_matches(request, "/api/blink")) {
        return method_not_allowed_response;
    }

    return not_found_response;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                       err_t err) {
    (void)arg;
    char request[HTTP_REQUEST_MAX];

    if (p == NULL) {
        return http_close(pcb);
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        return http_close(pcb);
    }

    tcp_recved(pcb, p->tot_len);
    const u16_t request_len =
        p->tot_len < sizeof(request) - 1 ? p->tot_len : sizeof(request) - 1;
    pbuf_copy_partial(p, request, request_len, 0);
    request[request_len] = '\0';
    pbuf_free(p);

    const char *response = http_response_for_request(request);

    err = tcp_write(pcb, response, strlen(response),
                    TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    err = tcp_output(pcb);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return http_close(pcb);
}

static err_t http_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void)arg;

    if (err != ERR_OK || new_pcb == NULL) {
        return ERR_VAL;
    }

    tcp_arg(new_pcb, NULL);
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
