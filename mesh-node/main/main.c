#include <cJSON.h>
#include <stdio.h>

#include "device.h"
#include "driver/gpio.h"
#include "esp_mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_indicator.h"
#include "mesh_node.h"
#include "sdkconfig.h"

#define MAX_DEVICES  6
#define RELAY_1      16
#define BUTTON_1     17
#define RELAY_2      18
#define BUTTON_2     19
#define RELAY_3      22
#define BUTTON_3     23
#define RELAY_4      25
#define BUTTON_4     26
#define RELAY_5      27
#define BUTTON_5     14
#define RELAY_6      32
#define BUTTON_6     33

#define RESET_BUTTON 4
#define RESET_BIT    6

#define RELAY_MASK                                                             \
    (1ULL << RELAY_1) | (1ULL << RELAY_2) | (1ULL << RELAY_3) |                \
        (1ULL << RELAY_4) | (1ULL << RELAY_5) | (1ULL << RELAY_6)
#define BUTTON_MASK                                                            \
    (1ULL << BUTTON_1) | (1ULL << BUTTON_2) | (1ULL << BUTTON_3) |             \
        (1ULL << BUTTON_4) | (1ULL << BUTTON_5) | (1ULL << BUTTON_6)

nvs_handle_t nvs_handler;

typedef struct {
    uint8_t relay_io;
    uint8_t button_io;
    int8_t  device_state;
    bool    is_handling;
} relay_device_t;

relay_device_t device_list[MAX_DEVICES];

void           define_devices() {
    device_list[0].relay_io    = RELAY_1;
    device_list[0].button_io   = BUTTON_1;
    device_list[0].is_handling = 0;

    device_list[1].relay_io    = RELAY_2;
    device_list[1].button_io   = BUTTON_2;
    device_list[1].is_handling = 0;

    device_list[2].relay_io    = RELAY_3;
    device_list[2].button_io   = BUTTON_3;
    device_list[2].is_handling = 0;

    device_list[3].relay_io    = RELAY_4;
    device_list[3].button_io   = BUTTON_4;
    device_list[3].is_handling = 0;

    device_list[4].relay_io    = RELAY_5;
    device_list[4].button_io   = BUTTON_5;
    device_list[4].is_handling = 0;

    device_list[5].relay_io    = RELAY_6;
    device_list[5].button_io   = BUTTON_6;
    device_list[5].is_handling = 0;
}

static EventGroupHandle_t event_group;

static void IRAM_ATTR     gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    int8_t   bit      = -1;
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        if (gpio_num == device_list[i].button_io) bit = i;
    }

    if (gpio_num == RESET_BUTTON) bit = RESET_BIT;

    if (bit != -1) {
        xEventGroupSetBitsFromISR(event_group, 1 << bit, NULL);
    }
}

static void check_reset_button() {
    uint8_t time_elapsed = 0;
    printf("Reset button down\n");
    for (;;) {
        vTaskDelay(100 / portTICK_PERIOD_MS);

        if (!gpio_get_level(RESET_BUTTON)) {
            if (++time_elapsed >= 30) {
                ESP_ERROR_CHECK(nvs_set_u8(nvs_handler, "is_configured", 0));
                ESP_ERROR_CHECK(nvs_set_u8(nvs_handler, "is_provisioned", 0));
                ESP_ERROR_CHECK(nvs_commit(nvs_handler));
                printf("Reset now\n");
                esp_restart();
                break;
            }
        } else {
            printf("Reset button up before enough time\n");
            break;
        }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
    xEventGroupClearBits(event_group, (1 << RESET_BIT));
}

void toggle_device(uint8_t num) {
    uint32_t button_io   = device_list[num].button_io;
    uint32_t relay_io    = device_list[num].relay_io;
    bool     is_handling = device_list[num].is_handling;
    if ((!gpio_get_level(button_io)) && (!is_handling)) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        if (!gpio_get_level(button_io)) {
            device_list[num].is_handling  = 1;
            device_list[num].device_state = 1 - device_list[num].device_state;
            gpio_set_level(relay_io, device_list[num].device_state);
            char temp[10];
            sprintf(temp, "relay_%d", num + 1);
            device_set_channel_value(temp, &(device_list[num].device_state));
            node_telemetry();
        }
    } else if ((gpio_get_level(button_io)) && (is_handling)) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        xEventGroupClearBits(event_group, 1 << num);
        device_list[num].is_handling = 0;
    }
}

static void soft_button_handler(void* arg) {
    EventBits_t uxBits;
    EventBits_t bitsToWaitFor = (1 << RESET_BIT);
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        if (device_list[i].device_state != -1) bitsToWaitFor |= (1 << i);
    }
    for (;;) {
        uxBits = xEventGroupWaitBits(event_group, bitsToWaitFor, pdFALSE,
                                     pdFALSE, portMAX_DELAY);

        for (uint8_t i = 0; i < MAX_DEVICES; i++) {
            if (device_list[i].device_state != -1) {
                if ((uxBits & (1 << i))) toggle_device(i);
            }
        }

        if (uxBits & (1 << RESET_BIT)) {
            check_reset_button();
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void config_gpio_init() {
    gpio_config_t relay_cfg = {.mode         = GPIO_MODE_INPUT_OUTPUT,
                               .pin_bit_mask = RELAY_MASK,
                               .pull_down_en = 0,
                               .pull_up_en   = 0};
    gpio_config(&relay_cfg);
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        gpio_set_level(device_list[i].relay_io, 0);
    }
}

uint8_t detect_connected_module() {
    gpio_config_t button_cfg = {.intr_type    = GPIO_INTR_NEGEDGE,
                                .mode         = GPIO_MODE_OUTPUT,
                                .pin_bit_mask = BUTTON_MASK,
                                .pull_down_en = 0,
                                .pull_up_en   = 1};
    gpio_config(&button_cfg);
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        gpio_set_level(device_list[i].button_io, 0);
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
    uint8_t count = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        gpio_set_direction(device_list[i].button_io, GPIO_MODE_INPUT);
        vTaskDelay(1 / portTICK_PERIOD_MS);
        if (!gpio_get_level(device_list[i].button_io)) {
            count++;
            device_list[i].device_state =
                gpio_get_level(device_list[i].relay_io);
        } else
            device_list[i].device_state = -1;
    }
    button_cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&button_cfg);
    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (device_list[i].device_state != -1)
            gpio_isr_handler_add(device_list[i].button_io, gpio_isr_handler,
                                 (void*)(device_list[i].button_io));
    }
    gpio_config_t reset_io_conf = {};
    reset_io_conf.intr_type     = GPIO_INTR_NEGEDGE;
    reset_io_conf.mode          = GPIO_MODE_INPUT;
    reset_io_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    reset_io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    reset_io_conf.pin_bit_mask  = (1ULL << RESET_BUTTON);
    gpio_config(&reset_io_conf);
    gpio_isr_handler_add(RESET_BUTTON, gpio_isr_handler, (void*)(RESET_BUTTON));

    return count;
}

void turn_on(gpio_num_t gpio_num) {
    if (!gpio_get_level(gpio_num)) {
        gpio_set_level(gpio_num, 1);
        for (uint8_t i = 0; i < MAX_DEVICES; i++) {
            if (gpio_num == device_list[i].relay_io)
                device_list[i].device_state = 1;
        }
    }
}

void turn_off(gpio_num_t gpio_num) {
    if (gpio_get_level(gpio_num)) {
        gpio_set_level(gpio_num, 0);
        for (uint8_t i = 0; i < MAX_DEVICES; i++) {
            if (gpio_num == device_list[i].relay_io)
                device_list[i].device_state = 0;
        }
    }
}

void turn_all_on() {
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        if (!device_list[i].device_state) turn_on(device_list[i].relay_io);
    }
}

void turn_all_off() {
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        if (device_list[i].device_state) turn_off(device_list[i].relay_io);
    }
}

void create_device_channel() {
    char temp[10];
    device_init("relay");
    for (uint8_t i = 0; i < MAX_DEVICES; i++) {
        sprintf(temp, "relay_%d", i + 1);
        if (device_list[i].device_state != -1) {
            device_add_bool_channel(temp, true, "", "");
        } else {
            device_add_bool_channel(temp, false, "", "");
        }
        device_set_channel_value(temp, &(device_list[i].device_state));
    }
}

static void mesh_node_receive(void* arg) {
    mesh_addr_t src;
    mesh_data_t recv_data;
    esp_err_t   err;
    int         flag            = 0;
    uint8_t     init_data[1024] = {0};
    cJSON*      data;
    cJSON*      action;
    recv_data.data = init_data;
    uint8_t is_provisioned;
    nvs_get_u8(nvs_handler, "is_provisioned", &is_provisioned);
    if (!is_provisioned) {
        for (;;) {
            memset(recv_data.data, 0x00, 1024);
            recv_data.size = 1024;
            err =
                esp_mesh_recv(&src, &recv_data, portMAX_DELAY, &flag, NULL, 0);
            if (err == ESP_OK) {
                data   = cJSON_Parse((char*)(recv_data.data));
                action = cJSON_GetObjectItem(data, "action");
                if (cJSON_IsString(action)) {
                    if (strcmp(action->valuestring, "provision") == 0) {
                        ESP_ERROR_CHECK(
                            nvs_set_u8(nvs_handler, "is_provisioned", 1));
                        ESP_ERROR_CHECK(nvs_commit(nvs_handler));
                        node_set_is_provisioned(true);
                        break;
                    }
                }
                if (cJSON_IsObject(data)) {
                    cJSON_Delete(data);
                }
            }
        }
    }
    printf("Start command reading\n");
    cJSON* relay_state = NULL;
    char   relay_temp[10];
    for (;;) {
        memset(recv_data.data, 0x00, 1024);
        recv_data.size = 1024;
        err = esp_mesh_recv(&src, &recv_data, portMAX_DELAY, &flag, NULL, 0);
        if (err == ESP_OK) {
            data = cJSON_Parse((char*)(recv_data.data));
            for (uint8_t i = 0; i < MAX_DEVICES; i++) {
                sprintf(relay_temp, "relay_%d", i + 1);
                relay_state = cJSON_GetObjectItem(data, relay_temp);
                if (cJSON_IsBool(relay_state)) {
                    if (relay_state->valueint) {
                        turn_on(device_list[i].relay_io);
                    } else {
                        turn_off(device_list[i].relay_io);
                    }
                    device_set_channel_value(relay_temp,
                                             &(device_list[i].device_state));
                }
            }
            if (cJSON_IsObject(data)) {
                node_telemetry();
                cJSON_Delete(data);
            }
        }
    }
}

void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("main", NVS_READWRITE, &nvs_handler));

    config_gpio_init();
    define_devices();
    detect_connected_module();
    event_group = xEventGroupCreate();
    xTaskCreate(soft_button_handler, "soft_button", 2048, NULL, 10, NULL);

    ind_led_init();
    node_config();

    xTaskCreate(mesh_node_receive, "receive", 8192, NULL, 5, NULL);

    create_device_channel();
    node_provision();
}
