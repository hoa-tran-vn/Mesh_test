#pragma once
#include "esp_mesh.h"
#include "nvs_flash.h"

extern nvs_handle_t nvs_handler;

void                root_config(void);
void                mqtt_receive_set_call_back(void* cb);
void                mqtt_root_publish(char* data);
void                root_provision();
void                root_set_is_provisioned(bool value);
void                root_telemetry();
void                send_to_node(mesh_addr_t node_addr, char* data);
char*               get_up_topic();
char*               get_down_topic();
char*               get_mac_addr_str();