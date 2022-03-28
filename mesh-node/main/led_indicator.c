#include "driver/ledc.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_indicator.h"

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          (5)
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_10_BIT
#define LEDC_DUTY               (1024)
#define LEDC_FREQUENCY          (50)

static void cb_ledc_fade_end_event(const ledc_cb_param_t *param, void *user_arg)
{
    static uint32_t target_duty = 0;
    target_duty = LEDC_DUTY - target_duty;
    if (param->event == LEDC_FADE_END_EVT) {
        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
                        LEDC_CHANNEL_0, target_duty, 500);
        ledc_fade_start(LEDC_LOW_SPEED_MODE,
                        LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT);
    }
}

void ind_led_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_DUTY_RES, 
        .freq_hz = LEDC_FREQUENCY,                      
        .speed_mode = LEDC_MODE,           
        .timer_num = LEDC_TIMER,            
        .clk_cfg = LEDC_AUTO_CLK,              
    };
    
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
            .channel    = LEDC_CHANNEL,
            .duty       = 0,
            .gpio_num   = LEDC_OUTPUT_IO,
            .speed_mode = LEDC_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER,
            .flags.output_invert = 0
    };
    ledc_channel_config(&ledc_channel);
    ledc_fade_func_install(0);
    ledc_cbs_t callbacks = {
        .fade_cb = cb_ledc_fade_end_event
    };
    ledc_cb_register(ledc_channel.speed_mode, ledc_channel.channel, &callbacks, NULL);
    ind_led_set_state(IND_LED_OFF);
}

void ind_led_set_state(ind_led_state_t state) {
    switch (state)
    {
        case IND_LED_OFF:
            ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
            ledc_set_freq(LEDC_MODE, LEDC_TIMER, 1);
            break;
        case IND_LED_WAIT_SMARTCONFIG:
            ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_set_freq(LEDC_MODE, LEDC_TIMER, LEDC_FREQUENCY);
            ledc_set_fade_with_time(LEDC_MODE,
                    LEDC_CHANNEL, 1, 500);
            ledc_fade_start(LEDC_MODE,
                            LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
            break;
        case IND_LED_WAIT_CONNECT_WIFI:
            ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (int)(LEDC_DUTY/2));
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
            ledc_set_freq(LEDC_MODE, LEDC_TIMER, 4);
            break;
        case IND_LED_WAIT_CONNECT_MESH:
            ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (int)(LEDC_DUTY/2));
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
            ledc_set_freq(LEDC_MODE, LEDC_TIMER, 1);
            break;
        case IND_LED_ON:
            ledc_stop(LEDC_MODE, LEDC_CHANNEL, 0);
            ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY);
            ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
            ledc_set_freq(LEDC_MODE, LEDC_TIMER, 1);
            break;
        default:
            break;
    }
}