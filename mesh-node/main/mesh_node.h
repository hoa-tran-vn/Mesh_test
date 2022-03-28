#pragma once
#include "nvs_flash.h"

extern nvs_handle_t nvs_handler;

void                node_config(void);
void                send_to_root(char* data);
void                node_provision(void);
void                node_set_is_provisioned(bool value);
void                node_telemetry(void);