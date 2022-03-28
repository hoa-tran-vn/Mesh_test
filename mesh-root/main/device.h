#pragma once
#include <cJSON.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#define INDICATOR_LED_GPIO      22
#define INDICATOR_LED_GPIO_MASK (1ULL << INDICATOR_LED_GPIO)

/* Channel data type */
typedef enum {
    CHANNEL_TYPE_BOOL,
    CHANNEL_TYPE_NUMBER,
    CHANNEL_TYPE_CHOICE,
    CHANNEL_TYPE_STRING,
} channel_type_t;

typedef struct prov_opt_list_t {
    struct prov_opt_list_t* next;
    char* opt;
} prov_opt_list_t;

typedef struct {
    float min;
    float max;
    float multipleof;
} prov_num_type_t;

typedef struct device_channel_t {
    struct device_channel_t* next;
    char* name;
    bool cmd;
    channel_type_t type;

    union {
        prov_num_type_t num_prov;
        prov_opt_list_t* opts_prov;
    } prov_data;

    union {
        bool bool_val;
        float num_val;
        char* str_val;
    } data_value;

} device_channel_t;

typedef struct {
    char* name;
    char* id;
    device_channel_t* channels;
} device_t;

/* Get device MAC address */
void get_device_id(char* id_buffer);

/* Check for MQTT provision status */
void device_is_mqtt_provisioned(bool* provisioned);

/* Set MQTT provision status */
void device_set_provisioned(void);

/* Check the provisioning response */
bool device_check_prov_resp(char* resp);

/* Create device structure */
void device_init(const char* device_name);

/* Add channel to device */
void device_add_bool_channel(const char* name,
                             bool cmd,
                             const char* title,
                             const char* description);

void device_add_nummber_channel(const char* name,
                                bool cmd,
                                const char* title,
                                const char* description,
                                float min,
                                float max,
                                float multipleof);

void device_add_multi_option_channel(const char* name,
                                     bool cmd,
                                     const char* title,
                                     const char* description,
                                     uint8_t opt_count,
                                     ...);

void device_add_string_channel(const char* name,
                               bool cmd,
                               const char* title,
                               const char* description);

/* Remove channel from device */
void device_remove_channel(const char* name);

/* Set channel value */
void device_set_channel_value(const char* name, void* value);

/* Get the JSON provisioning data */
char* device_get_mqtt_provision_json_data(void);

/* Get the JSON sate data */
char* device_get_mqtt_state_json_data(void);

/* Print device created channels */
void print_device_channels(void);

/* Indicator LED */
void indicator_led_init(void);
void indicator_led_on(void);