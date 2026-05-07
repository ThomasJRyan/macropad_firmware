#ifndef MACROPAD_REST_CLIENT_H
#define MACROPAD_REST_CLIENT_H

#include <stddef.h>

void rest_client_init(void);
void rest_client_poll(void);
void rest_client_trigger(size_t button_index);

#endif
