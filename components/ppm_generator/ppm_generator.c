#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/timer.h"
#include "soc/soc_caps.h"
#include "ppm_generator.h"

typedef union {
    struct {
        uint32_t reserved0:   10;
        uint32_t alarm_en:     1;             /*When set  alarm is enabled*/
        uint32_t level_int_en: 1;             /*When set  level type interrupt will be generated during alarm*/
        uint32_t edge_int_en:  1;             /*When set  edge type interrupt will be generated during alarm*/
        uint32_t divider:     16;             /*Timer clock (T0/1_clk) pre-scale value.*/
        uint32_t autoreload:   1;             /*When set  timer 0/1 auto-reload at alarming is enabled*/
        uint32_t increase:     1;             /*When set  timer 0/1 time-base counter increment. When cleared timer 0 time-base counter decrement.*/
        uint32_t enable:       1;             /*When set timer 0/1 time-base counter is enabled*/
    };
    uint32_t val;
} timer_cfg_t;

typedef struct hw_timer_s
{
    uint8_t group;
    uint8_t num;
} hw_timer_t;

static hw_timer_t timer_dev[4] = {
    {0,0}, {1,0},  {0,1},  {1,1}
};

typedef struct apb_change_cb_s {
        struct apb_change_cb_s * prev;
        struct apb_change_cb_s * next;
        void * arg;
        apb_change_cb_t cb;
} apb_change_t;

static apb_change_t * apb_change_callbacks = NULL;
static SemaphoreHandle_t apb_change_lock = NULL;

static void initApbChangeCallback(){
    static volatile bool initialized = false;
    if(!initialized){
        initialized = true;
        apb_change_lock = xSemaphoreCreateMutex();
        if(!apb_change_lock){
            initialized = false;
        }
    }
}

bool addApbChangeCallback(void * arg, apb_change_cb_t cb){
    initApbChangeCallback();
    apb_change_t * c = (apb_change_t*)malloc(sizeof(apb_change_t));
    if(!c){
        return false;
    }                 
    c->next = NULL;
    c->prev = NULL;
    c->arg = arg;
    c->cb = cb;
    xSemaphoreTake(apb_change_lock, portMAX_DELAY);
    if(apb_change_callbacks == NULL){
        apb_change_callbacks = c;
    } else {
        apb_change_t * r = apb_change_callbacks;
        // look for duplicate callbacks
        while( (r != NULL ) && !((r->cb == cb) && ( r->arg == arg))) r = r->next;
        if (r) {
            free(c);
            xSemaphoreGive(apb_change_lock);
            return false;
        }
        else {
            c->next = apb_change_callbacks;
            apb_change_callbacks-> prev = c;
            apb_change_callbacks = c;
        }
    }
    xSemaphoreGive(apb_change_lock);
    return true;
}

uint32_t timerGetConfig(hw_timer_t *timer){
    timer_config_t timer_cfg;
    timer_get_config(timer->group, timer->num,&timer_cfg);

    //Translate to default uint32_t
    timer_cfg_t cfg;
    cfg.alarm_en = timer_cfg.alarm_en;
    cfg.autoreload = timer_cfg.auto_reload;
    cfg.divider = timer_cfg.divider;
    cfg.edge_int_en = timer_cfg.intr_type;
    cfg.level_int_en = !timer_cfg.intr_type;
    cfg.enable = timer_cfg.counter_en;
    cfg.increase = timer_cfg.counter_dir;

    return cfg.val;
}

bool IRAM_ATTR timerFnWrapper(void *arg){
    void (*fn)(void) = arg;
    fn();

    // some additional logic or handling may be required here to approriately yield or not
    return false;
}

void timerAttachInterruptFlag(hw_timer_t *timer, void (*fn)(void), bool edge, int intr_alloc_flags){
    timer_isr_callback_add(timer->group, timer->num, timerFnWrapper, fn, intr_alloc_flags);
}

void timerAttachInterrupt(hw_timer_t *timer, void (*fn)(void), bool edge){
    timerAttachInterruptFlag(timer, fn, edge, 0);
}

void timerSetDivider(hw_timer_t *timer, uint16_t divider){
    timer_set_divider(timer->group, timer->num,divider);
}

void timerSetAutoReload(hw_timer_t *timer, bool autoreload){
    timer_set_auto_reload(timer->group, timer->num,autoreload);
}

void timerStart(hw_timer_t *timer){
    timer_start(timer->group, timer->num);
}

void timerStop(hw_timer_t *timer){
    timer_pause(timer->group, timer->num);
}

void timerAlarmEnable(hw_timer_t *timer){
    timer_set_alarm(timer->group, timer->num,true);
}

void timerAlarmWrite(hw_timer_t *timer, uint64_t alarm_value, bool autoreload){
    timer_set_alarm_value(timer->group, timer->num, alarm_value);
    timerSetAutoReload(timer,autoreload);
}

uint16_t timerGetDivider(hw_timer_t *timer){
    timer_cfg_t config;
    config.val = timerGetConfig(timer);
    return config.divider;
}

static void _on_apb_change(void * arg, apb_change_ev_t ev_type, uint32_t old_apb, uint32_t new_apb){
    hw_timer_t * timer = (hw_timer_t *)arg;
    if(ev_type == APB_BEFORE_CHANGE){
        timerStop(timer);
    } else {
        old_apb /= 1000000;
        new_apb /= 1000000;
        uint16_t divider = (new_apb * timerGetDivider(timer)) / old_apb;
        timerSetDivider(timer,divider);
        timerStart(timer);
    }
}

hw_timer_t * ppm_init(void){
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,   // Disable interrupt
        .mode = GPIO_MODE_OUTPUT,         // Set as output mode
        .pin_bit_mask = (1ULL << GPIO_NUM_15), // Bit mask of the pins that you want to set
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // Disable pull-down mode
        .pull_up_en = GPIO_PULLUP_DISABLE  // Disable pull-up mode
    };   
    gpio_config(&io_conf);

    hw_timer_t * timer = &timer_dev[0];   //Get Timer group/num from 0-3 number
    
    timer_config_t config = {
        .divider = 80,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_DIS,
        .auto_reload = false,
    };

    timer_init(timer->group, timer->num, &config);
    timer_set_counter_value(timer->group, timer->num, 0);
    timerStart(timer);
    addApbChangeCallback(timer, _on_apb_change);
    return timer;
}


