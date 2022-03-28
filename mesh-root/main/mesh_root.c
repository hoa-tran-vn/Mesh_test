#include "mesh_root.h"

#include <cJSON.h>
#include <mqtt_client.h>
#include <string.h>

#include "device.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "led_indicator.h"
#include "nvs_flash.h"

// #define CONFIG_MESH_IE_CRYPTO_KEY "topsecret"
#define CONFIG_MESH_NON_MESH_AP_CONNECTIONS 0
#define CONFIG_MESH_AP_AUTHMODE             2
#define CONFIG_MESH_AP_CONNECTIONS          6
#define CONFIG_MESH_AP_PASSWD               "topsecret"
#define CONFIG_MESH_TOPOLOGY                0
#define MQTT_BROKER_ADDRESS                 "mqtt://172.29.5.92"
// #define MQTT_BROKER_ADDRESS                 "mqtt://mqtt.eclipseprojects.io"
#define MQTT_CONNECTED_BIT                  (1 << 15)

esp_netif_t *                   sta_netif;
uint8_t                         is_configured;
uint8_t                         is_provisioned;
wifi_config_t                   wifi_config;
char                            wifi_ssid[32];
char                            wifi_pswd[64];

mesh_addr_t                     mesh_parent_addr;
int                             mesh_layer     = -1;

static bool                     mqtt_connected = false;
static bool                     wifi_connected = false;
static char *                   up_topic;
static char *                   down_topic;
static char *                   mac_addr_str;
static EventGroupHandle_t       event_group;
static esp_mqtt_client_handle_t mqtt_client;
void (*input_call_back)(char *topic, char *data) = NULL;

void MQTT_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data) {
    char                    MQTT_TAG[] = "MQTT";
    esp_mqtt_event_handle_t event      = (esp_mqtt_event_handle_t)event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            ind_led_set_state(IND_LED_ON);
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_CONNECTED");
            xEventGroupSetBits(event_group, MQTT_CONNECTED_BIT);
            esp_mqtt_client_subscribe(mqtt_client, down_topic, 0);
            mqtt_connected = true;
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            if (wifi_connected) {
                ind_led_set_state(IND_LED_WAIT_CONNECT_MQTT);
            }
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_SUBSCRIBED: {
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED");
            break;
        }
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED");
            break;
        case MQTT_EVENT_DATA: {
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA");

            /* Allocate memory for data */
            char *topic = malloc(event->topic_len + 1);
            char *data  = malloc(event->data_len + 1);

            sprintf(topic, "%.*s", event->topic_len, event->topic);
            sprintf(data, "%.*s", event->data_len, event->data);

            /* Handle received data */
            input_call_back(topic, data);

            /* Free memory after handling data */
            free(topic);
            free(data);
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);
            break;
    }
}

static void mesh_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    mesh_addr_t id         = {0};
    uint16_t    last_layer = 0;
    char        MESH_TAG[] = "MESH";

    switch (event_id) {
        case MESH_EVENT_STARTED: {
            esp_mesh_get_id(&id);
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:" MACSTR "",
                     MAC2STR(id.addr));
            mesh_layer = esp_mesh_get_layer();
        } break;
        case MESH_EVENT_STOPPED: {
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
            mesh_layer = esp_mesh_get_layer();
        } break;
        case MESH_EVENT_CHILD_CONNECTED: {
            mesh_event_child_connected_t *child_connected =
                (mesh_event_child_connected_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, " MACSTR "",
                     child_connected->aid, MAC2STR(child_connected->mac));
        } break;
        case MESH_EVENT_CHILD_DISCONNECTED: {
            mesh_event_child_disconnected_t *child_disconnected =
                (mesh_event_child_disconnected_t *)event_data;
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, " MACSTR "",
                     child_disconnected->aid, MAC2STR(child_disconnected->mac));
        } break;
        case MESH_EVENT_ROUTING_TABLE_ADD: {
            mesh_event_routing_table_change_t *routing_table =
                (mesh_event_routing_table_change_t *)event_data;
            ESP_LOGW(MESH_TAG,
                     "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d, layer:%d",
                     routing_table->rt_size_change, routing_table->rt_size_new,
                     mesh_layer);
        } break;
        case MESH_EVENT_ROUTING_TABLE_REMOVE: {
            mesh_event_routing_table_change_t *routing_table =
                (mesh_event_routing_table_change_t *)event_data;
            ESP_LOGW(
                MESH_TAG,
                "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d, layer:%d",
                routing_table->rt_size_change, routing_table->rt_size_new,
                mesh_layer);
        } break;
        case MESH_EVENT_NO_PARENT_FOUND: {
            mesh_event_no_parent_found_t *no_parent =
                (mesh_event_no_parent_found_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                     no_parent->scan_times);
        }
        /* TODO handler for the failure */
        break;
        case MESH_EVENT_PARENT_CONNECTED: {
            mesh_event_connected_t *connected =
                (mesh_event_connected_t *)event_data;
            esp_mesh_get_id(&id);
            mesh_layer = connected->self_layer;
            memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
            ESP_LOGI(
                MESH_TAG,
                "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:" MACSTR
                "%s, ID:" MACSTR ", duty:%d",
                last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                esp_mesh_is_root()  ? "<ROOT>"
                : (mesh_layer == 2) ? "<layer2>"
                                    : "",
                MAC2STR(id.addr), connected->duty);
            last_layer = mesh_layer;
            if (esp_mesh_is_root()) {
                esp_netif_dhcpc_stop(sta_netif);
                esp_netif_dhcpc_start(sta_netif);
            }
        } break;
        case MESH_EVENT_PARENT_DISCONNECTED: {
            mesh_event_disconnected_t *disconnected =
                (mesh_event_disconnected_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                     disconnected->reason);
            mesh_layer = esp_mesh_get_layer();
        } break;
        case MESH_EVENT_LAYER_CHANGE: {
            mesh_event_layer_change_t *layer_change =
                (mesh_event_layer_change_t *)event_data;
            mesh_layer = layer_change->new_layer;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                     last_layer, mesh_layer,
                     esp_mesh_is_root()  ? "<ROOT>"
                     : (mesh_layer == 2) ? "<layer2>"
                                         : "");
            last_layer = mesh_layer;
        } break;
        case MESH_EVENT_ROOT_ADDRESS: {
            mesh_event_root_address_t *root_addr =
                (mesh_event_root_address_t *)event_data;
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_ROOT_ADDRESS>root address:" MACSTR "",
                     MAC2STR(root_addr->addr));
        } break;
        case MESH_EVENT_VOTE_STARTED: {
            mesh_event_vote_started_t *vote_started =
                (mesh_event_vote_started_t *)event_data;
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, "
                     "rc_addr:" MACSTR "",
                     vote_started->attempts, vote_started->reason,
                     MAC2STR(vote_started->rc_addr.addr));
        } break;
        case MESH_EVENT_VOTE_STOPPED: {
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
            break;
        }
        case MESH_EVENT_ROOT_SWITCH_REQ: {
            mesh_event_root_switch_req_t *switch_req =
                (mesh_event_root_switch_req_t *)event_data;
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:" MACSTR
                     "",
                     switch_req->reason, MAC2STR(switch_req->rc_addr.addr));
        } break;
        case MESH_EVENT_ROOT_SWITCH_ACK: {
            /* new root */
            mesh_layer = esp_mesh_get_layer();
            esp_mesh_get_parent_bssid(&mesh_parent_addr);
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:" MACSTR "",
                     mesh_layer, MAC2STR(mesh_parent_addr.addr));
        } break;
        case MESH_EVENT_TODS_STATE: {
            mesh_event_toDS_state_t *toDs_state =
                (mesh_event_toDS_state_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d",
                     *toDs_state);
        } break;
        case MESH_EVENT_ROOT_FIXED: {
            mesh_event_root_fixed_t *root_fixed =
                (mesh_event_root_fixed_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                     root_fixed->is_fixed ? "fixed" : "not fixed");
        } break;
        case MESH_EVENT_ROOT_ASKED_YIELD: {
            mesh_event_root_conflict_t *root_conflict =
                (mesh_event_root_conflict_t *)event_data;
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_ROOT_ASKED_YIELD>" MACSTR
                     ", rssi:%d, capacity:%d",
                     MAC2STR(root_conflict->addr), root_conflict->rssi,
                     root_conflict->capacity);
        } break;
        case MESH_EVENT_CHANNEL_SWITCH: {
            mesh_event_channel_switch_t *channel_switch =
                (mesh_event_channel_switch_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d",
                     channel_switch->channel);
        } break;
        case MESH_EVENT_SCAN_DONE: {
            mesh_event_scan_done_t *scan_done =
                (mesh_event_scan_done_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                     scan_done->number);
        } break;
        case MESH_EVENT_NETWORK_STATE: {
            mesh_event_network_state_t *network_state =
                (mesh_event_network_state_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                     network_state->is_rootless);
        } break;
        case MESH_EVENT_STOP_RECONNECTION: {
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
        } break;
        case MESH_EVENT_FIND_NETWORK: {
            mesh_event_find_network_t *find_network =
                (mesh_event_find_network_t *)event_data;
            ESP_LOGI(
                MESH_TAG,
                "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:" MACSTR
                "",
                find_network->channel, MAC2STR(find_network->router_bssid));
        } break;
        case MESH_EVENT_ROUTER_SWITCH: {
            mesh_event_router_switch_t *router_switch =
                (mesh_event_router_switch_t *)event_data;
            ESP_LOGI(
                MESH_TAG,
                "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, " MACSTR
                "",
                router_switch->ssid, router_switch->channel,
                MAC2STR(router_switch->bssid));
        } break;
        case MESH_EVENT_PS_PARENT_DUTY: {
            mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_PS_PARENT_DUTY>duty:%d",
                     ps_duty->duty);
        } break;
        case MESH_EVENT_PS_CHILD_DUTY: {
            mesh_event_ps_duty_t *ps_duty = (mesh_event_ps_duty_t *)event_data;
            ESP_LOGI(MESH_TAG,
                     "<MESH_EVENT_PS_CHILD_DUTY>cidx:%d, " MACSTR ", duty:%d",
                     ps_duty->child_connected.aid - 1,
                     MAC2STR(ps_duty->child_connected.mac), ps_duty->duty);
        } break;
        default:
            ESP_LOGI(MESH_TAG, "unknown id:%d", event_id);
            break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
    wifi_connected = true;
    ind_led_set_state(IND_LED_WAIT_CONNECT_MQTT);
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI("IP", "<IP_EVENT_STA_GOT_IP>IP:" IPSTR,
             IP2STR(&event->ip_info.ip));
    if (is_configured == 0) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handler, "wifi_ssid", wifi_ssid));
        ESP_ERROR_CHECK(nvs_set_str(nvs_handler, "wifi_pswd", wifi_pswd));
        ESP_ERROR_CHECK(nvs_set_u8(nvs_handler, "is_configured", 1));
        ESP_ERROR_CHECK(nvs_commit(nvs_handler));
        // reset
        esp_restart();
    }
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = MQTT_BROKER_ADDRESS,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                   MQTT_event_handler, mqtt_client);
    esp_mqtt_client_start(mqtt_client);
    ESP_ERROR_CHECK(esp_event_handler_register("MQTT_EVENTS", ESP_EVENT_ANY_ID,
                                               &MQTT_event_handler, NULL));
}

static void sc_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
    switch (event_id) {
        case SC_EVENT_GOT_SSID_PSWD: {
            ESP_LOGI("SMARTCONFIG", "Got SSID and PSWD");
            smartconfig_event_got_ssid_pswd_t *evt =
                (smartconfig_event_got_ssid_pswd_t *)event_data;

            if (evt->type == SC_TYPE_ESPTOUCH_V2) {
                uint8_t rvd_data[33] = {0};
                ESP_ERROR_CHECK(
                    esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)));
                printf("rvd_data = %s\n", rvd_data);
            }

            memcpy(wifi_ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
            memcpy(wifi_pswd, evt->password, sizeof(wifi_config.sta.password));

            memcpy(wifi_config.sta.ssid, wifi_ssid,
                   sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, wifi_pswd,
                   sizeof(wifi_config.sta.password));
            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            esp_wifi_connect();
        } break;
        case SC_EVENT_SEND_ACK_DONE: {
            ESP_LOGI("SMARTCONFIG",
                     "<SC_EVENT_SEND_ACK_DONE> smartconfig over");
            esp_smartconfig_stop();
        } break;
        default:
            ESP_LOGI("SMARTCONFIG", "unknown id: %d", event_id);
            break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    switch (event_id) {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI("WIFI", "<WIFI_EVENT_STA_START>");
            nvs_get_u8(nvs_handler, "is_configured", &is_configured);
            if (is_configured == 1) {
                ind_led_set_state(IND_LED_WAIT_CONNECT_WIFI);
                esp_netif_destroy_default_wifi(sta_netif);
                ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(
                    &sta_netif, NULL));
                ESP_ERROR_CHECK(esp_mesh_init());
                ESP_ERROR_CHECK(esp_event_handler_register(
                    MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

                // notify here MOREEEE
                uint8_t MESH_ID[6];
                esp_read_mac(MESH_ID, ESP_MAC_WIFI_STA);

                mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
                memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, 6);
                cfg.channel      = 0;  // 0 for autoscan

                size_t ssid_size = sizeof(wifi_ssid);
                size_t pswd_size = sizeof(wifi_pswd);
                nvs_get_str(nvs_handler, "wifi_ssid", wifi_ssid, &ssid_size);
                nvs_get_str(nvs_handler, "wifi_pswd", wifi_pswd, &pswd_size);
                cfg.router.ssid_len = ssid_size;
                memcpy((uint8_t *)&cfg.router.ssid, wifi_ssid, ssid_size);
                memcpy((uint8_t *)&cfg.router.password, wifi_pswd, pswd_size);

                ESP_ERROR_CHECK(
                    esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
                cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
                cfg.mesh_ap.nonmesh_max_connection =
                    CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
                memcpy((uint8_t *)&cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
                       strlen(CONFIG_MESH_AP_PASSWD));
                ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

                memcpy(wifi_config.sta.ssid, wifi_ssid,
                       sizeof(wifi_config.sta.ssid));
                memcpy(wifi_config.sta.password, wifi_pswd,
                       sizeof(wifi_config.sta.password));
                ESP_ERROR_CHECK(esp_mesh_set_parent(
                    &wifi_config, NULL, MESH_ROOT, MESH_ROOT_LAYER));
                ESP_ERROR_CHECK(esp_mesh_start());
            } else {
                ind_led_set_state(IND_LED_WAIT_SMARTCONFIG);
                ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_V2));
                smartconfig_start_config_t cfg =
                    SMARTCONFIG_START_CONFIG_DEFAULT();
                ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
            }
        } break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_connected = false;
            ind_led_set_state(IND_LED_WAIT_CONNECT_WIFI);
            ESP_LOGI("WIFI", "<WIFI_EVENT_STA_DISCONNECTED>");
        } break;
        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI("WIFI", "<WIFI_EVENT_STA_CONNECTED>");
            break;
        }
        default:
            ESP_LOGI("WIFI", "other events: %d", event_id);
            break;
    }
}

void root_config(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    uint8_t eth_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    mac_addr_str = malloc(13);
    sprintf(mac_addr_str, "%02X%02X%02X%02X%02X%02X", MAC2STR(eth_mac));
    up_topic = malloc(20);
    sprintf(up_topic, "up/MAC/%s", mac_addr_str);
    down_topic = malloc(22);
    sprintf(down_topic, "down/MAC/%s", mac_addr_str);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                               &sc_event_handler, NULL));

    event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(event_group, MQTT_CONNECTED_BIT, true, false,
                        portMAX_DELAY);
}

void mqtt_receive_set_call_back(void *cb) { input_call_back = cb; }

void mqtt_root_publish(char *data) {
    if (mqtt_connected) {
        esp_mqtt_client_publish(mqtt_client, up_topic, data, 0, 1, 0);
    }
}

void root_provision() {
    if (mqtt_connected) {
        nvs_get_u8(nvs_handler, "is_provisioned", &is_provisioned);
        if (!is_provisioned) {
            while (is_provisioned != 1) {
                char *mqtt_prov_data = device_get_mqtt_provision_json_data();
                esp_mqtt_client_publish(mqtt_client, up_topic, mqtt_prov_data,
                                        0, 1, 0);
                vTaskDelay(30000 / portTICK_PERIOD_MS);
            }
        }
    }
}

void root_set_is_provisioned(bool value) { is_provisioned = value; }

void root_telemetry() {
    if (mqtt_connected) {
        char *mqtt_tele_data = device_get_mqtt_state_json_data();
        esp_mqtt_client_publish(mqtt_client, up_topic, mqtt_tele_data, 0, 1, 0);
    }
}

void send_to_node(mesh_addr_t node_addr, char *data) {
    mesh_data_t send_data;
    send_data.data  = (uint8_t *)data;
    send_data.size  = strlen(data);
    send_data.proto = MESH_PROTO_BIN;
    send_data.tos   = MESH_DATA_FROMDS;
    esp_mesh_send(&node_addr, &send_data, MESH_DATA_FROMDS, NULL, 0);
}

char *get_up_topic() { return up_topic; }

char *get_down_topic() { return down_topic; }

char *get_mac_addr_str() { return mac_addr_str; }