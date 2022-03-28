#pragma once

typedef enum {
    IND_LED_OFF,
    IND_LED_WAIT_SMARTCONFIG,
    IND_LED_WAIT_CONNECT_WIFI,
    IND_LED_WAIT_CONNECT_MESH,
    IND_LED_ON
} ind_led_state_t;

void ind_led_init(void);
void ind_led_set_state(ind_led_state_t state);