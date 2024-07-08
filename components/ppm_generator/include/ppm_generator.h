#include <stdint.h>

struct hw_timer_s;
typedef struct hw_timer_s hw_timer_t;

typedef enum { APB_BEFORE_CHANGE, APB_AFTER_CHANGE } apb_change_ev_t;
typedef void (* apb_change_cb_t)(void * arg, apb_change_ev_t ev_type, uint32_t old_apb, uint32_t new_apb);

hw_timer_t * ppm_init(void);
void timerAlarmWrite(hw_timer_t *timer, uint64_t alarm_value, bool autoreload);
void timerAttachInterrupt(hw_timer_t *timer, void (*fn)(void), bool edge);
void timerAlarmEnable(hw_timer_t *timer);

