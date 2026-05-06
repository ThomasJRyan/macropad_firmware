#ifndef MACROPAD_BLINK_REQUEST_H
#define MACROPAD_BLINK_REQUEST_H

#include <stdbool.h>

void blink_request_init(void);
void blink_request_enqueue_web(void);
bool blink_request_take_web(void);

#endif
