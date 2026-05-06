#ifndef MACROPAD_NETWORK_H
#define MACROPAD_NETWORK_H

#include "lwip/err.h"

#define MACROPAD_AP_SSID "macropad_setup"

err_t network_start(void);

#endif
