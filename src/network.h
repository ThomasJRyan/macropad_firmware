#ifndef MACROPAD_NETWORK_H
#define MACROPAD_NETWORK_H

#include "lwip/err.h"

#define MACROPAD_AP_SSID "macropad_setup"
#define MACROPAD_MDNS_HOSTNAME "macropad"

typedef enum {
    NETWORK_MODE_SETUP_AP,
    NETWORK_MODE_STATION,
} network_mode_t;

typedef enum {
    NETWORK_START_SETUP_AP,
    NETWORK_START_STATION_CONNECTED,
    NETWORK_START_STATION_FAILED,
} network_start_status_t;

typedef struct {
    err_t err;
    network_mode_t mode;
    network_start_status_t status;
} network_start_result_t;

network_start_result_t network_start(void);
void network_debug_poll(void);

#endif
