#include "blink_request.h"

#include "pico/critical_section.h"

static critical_section_t blink_request_lock;
static bool web_blink_pending;

void blink_request_init(void) {
    critical_section_init(&blink_request_lock);
}

void blink_request_enqueue_web(void) {
    critical_section_enter_blocking(&blink_request_lock);
    web_blink_pending = true;
    critical_section_exit(&blink_request_lock);
}

bool blink_request_take_web(void) {
    critical_section_enter_blocking(&blink_request_lock);
    const bool pending = web_blink_pending;
    web_blink_pending = false;
    critical_section_exit(&blink_request_lock);

    return pending;
}
