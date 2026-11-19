/* Coupled production AP-wake reset/hold/timer regression.
 * Boundary stubs exclude full PMU/board delivery and real guest qualification.
 * SLICE:defs include/hw/arm/ipod_touch_buttons.h file
 * SLICE:defs include/hw/arm/ipod_touch_gpio.h define GPIO_BUTTON_|NUM_GPIO_|GPIO2
 * SLICE:defs contrib/ios-app/qemu-ios-ui.h define QEMU_IOS_BUTTON_|QEMU_IOS_TOUCH_
 * SLICE:types include/hw/arm/ipod_touch_gpio.h typedef IPodTouchGPIOState
 * SLICE:gpio hw/arm/ipod_touch_gpio.c fn gpio_is_on gpio_is_off gpio_set_on gpio_set_off ipod_touch_gpio_record_host_input s5l8900_gpio_reset s5l8900_gpio_reset_exit
 * SLICE:buttons contrib/ios-app/qemu-ios-ui.c range struct ios_button { | void qemu_ios_ui_button(
 * SLICE:reset hw/arm/ipod_touch_2g.c fn ipod_touch_ap_wake_reset_pending
 * SLICE:reset contrib/ios-app/qemu-ios-ui.c fn portable_release_input portable_release_all ios_sequence_reset
 * CFLAGS -Wall -Werror -Wno-unused-parameter
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int SysBusDevice, MemoryRegion, ResetType;
typedef void DeviceState, Object;
typedef int *qemu_irq;
#include "defs.h"
#include "types.h"
#define IPOD_TOUCH_GPIO(o) ((IPodTouchGPIOState *)(o))
#define QEMU_CLOCK_VIRTUAL 0
#define MAX(a,b) ((a)>(b)?(a):(b))
#define g_free free
static IPodTouchGPIOState gpio;
static struct { IPodTouchGPIOState *gpio_state; } mt = { &gpio }, *s_kbd_mt = &mt;
static void qemu_set_irq(qemu_irq line,int level) { if(line) *line=level; }
#include "gpio.h"
typedef struct { void (*cb)(void *); void *arg; int64_t deadline; bool pending; } QEMUTimer;
static int64_t now;
static int64_t qemu_clock_get_ms(int clock) { return now; }
static QEMUTimer *timer_new_ms(int clock,void (*cb)(void *),void *arg) {
    QEMUTimer *t=calloc(1,sizeof(*t)); assert(t); t->cb=cb; t->arg=arg; return t;
}
static void timer_del(QEMUTimer *t) { t->pending=false; }
static void timer_mod(QEMUTimer *t,int64_t deadline) { t->deadline=deadline; t->pending=true; }
static uint32_t pin_for(IPodTouchButton b) {
    static const uint32_t pins[]={GPIO_BUTTON_HOME,GPIO_BUTTON_POWER,GPIO_BUTTON_VOLUP,GPIO_BUTTON_VOLDOWN};
    assert(b>=0 && b<4); return pins[b];
}
/* Board boundary only: actual full key_event/PMU IRQ behavior is NOT sliced.
 * Active-low volume recording here cannot test the production keycode mapping. */
void ipod_touch_press_button(IPodTouchButton b,bool down) {
    uint32_t pin=pin_for(b); bool high=b<2?down:!down;
    ipod_touch_gpio_record_host_input(&gpio,pin,high);
    if(high) gpio_set_on(gpio.gpio_state,pin); else gpio_set_off(gpio.gpio_state,pin);
}
void ipad1_press_button(IPodTouchButton b,bool down) {}
void ipod_touch_1g_press_button(IPodTouchButton b,bool down) {}
void s5l8920_press_button(IPodTouchButton b,bool down) {}
static bool automated_power;
static unsigned sequence_cancel_calls;
/* Sequence dependency boundary: separate existing sequence tests cover its body. */
static void ios_sequence_cancel_current(void) {
    sequence_cancel_calls++;
    if(automated_power) { ipod_touch_press_button(IPOD_TOUCH_BUTTON_POWER,false); automated_power=false; }
}
#include "buttons.h"
typedef struct { int button; bool down; } InputBtnEvent;
typedef struct { int type; union { struct { InputBtnEvent *data; } btn; } u; } InputEvent;
typedef struct { bool motion; } LIS302DLState;
#define INPUT_BUTTON_LEFT 0
#define INPUT_EVENT_KIND_BTN 0
static struct { void *con; } ios = { &gpio };
static struct { bool active; } portable_contacts[5];
static bool ios_manual_touch,ios_manual_touch2;
static uint64_t portable_touch_timestamp,portable_motion_timestamp[2];
static LIS302DLState accel;
static unsigned touch_ends,syncs;
static LIS302DLState *portable_accel(void) { return &accel; }
static void lis302dl_clear_host_motion(LIS302DLState *a) { a->motion=false; }
static void portable_emit(int slot,int phase,double x,double y) { assert(phase==QEMU_IOS_TOUCH_END); touch_ends++; }
static void qemu_input_event_send_impl(void *con,InputEvent *e) { assert(!e->u.btn.data->down); touch_ends++; }
static void qemu_input_event_sync_impl(void) { syncs++; }
#include "reset.h"
static void event(int b,bool down) {
    struct ios_button *e=calloc(1,sizeof(*e)); assert(e); e->button=b; e->down=down; ios_button_bh(e);
}
static void advance(int64_t t) {
    now=t;
    for(int b=0;b<4;b++) {
        QEMUTimer *timer=ios_button_holds[b].release;
        if(timer && timer->pending && t>=timer->deadline) { timer->pending=false; timer->cb(timer->arg); }
    }
}
static void reset_holds(bool gpio_first,bool wake) {
    gpio.ap_wake_reset_pending=wake;
    if(gpio_first) { s5l8900_gpio_reset((DeviceState *)&gpio); ios_sequence_reset(NULL); }
    else { ios_sequence_reset(NULL); s5l8900_gpio_reset((DeviceState *)&gpio); }
    assert(gpio.ap_wake_reset_pending==wake);
    s5l8900_gpio_reset_exit((Object *)&gpio,0);
    assert(!gpio.ap_wake_reset_pending);
}
static void cleanup(void) {
    portable_release_all();
    for(int b=0;b<4;b++) { free(ios_button_holds[b].release); memset(&ios_button_holds[b],0,sizeof(ios_button_holds[b])); }
    memset(&gpio,0,sizeof(gpio)); gpio.rest_high_pad=NUM_GPIO_PADS;
    automated_power=false;
}
int main(void) {
    cleanup();
    for(int order=0;order<2;order++) for(int b=0;b<4;b++) {
        now=1000; event(b,true); event(b,false);
        QEMUTimer *timer=ios_button_holds[b].release;
        assert(timer->pending && timer->deadline==1100);
        portable_contacts[0].active=true; ios_manual_touch=true; accel.motion=true;
        portable_touch_timestamp=17; portable_motion_timestamp[0]=18; touch_ends=0;
        gpio.gpio_state[2]=0xff; gpio.gpio_state[0]=0x80;
        now=1050; reset_holds(order,true);
        assert(gpio_is_on(gpio.gpio_state,pin_for(b))==(b<2) && ios_button_holds[b].down);
        assert(gpio.gpio_state[2]==0 && gpio.gpio_state[0]==1);
        assert(ios_button_holds[b].pressed_at==1000 && timer->pending && timer->deadline==1100);
        assert(!portable_contacts[0].active && !ios_manual_touch && !accel.motion && touch_ends==2);
        assert(!portable_touch_timestamp && !portable_motion_timestamp[0]);
        advance(1099); assert(gpio_is_on(gpio.gpio_state,pin_for(b))==(b<2));
        advance(1100); assert(gpio_is_on(gpio.gpio_state,pin_for(b))==(b>=2) && !ios_button_holds[b].down);
        reset_holds(order,true); assert(gpio_is_on(gpio.gpio_state,pin_for(b))==(b>=2));
        cleanup();
    }
    for(int order=0;order<2;order++) {
        now=1000; event(1,true); event(1,false); now=1050;
        reset_holds(order,false);
        assert(!ios_button_holds[1].down && !ios_button_holds[1].release->pending);
        assert(!gpio_is_on(gpio.gpio_state,GPIO_BUTTON_POWER));
        // A later UI cancellation can record a released (low) power input;
        // that is not a stale DOWN and need not have an empty mask.
        assert(!(gpio.host_input_level[GPIO2PAD(GPIO_BUTTON_POWER)] & (1u<<GPIO2PIN(GPIO_BUTTON_POWER))));
        reset_holds(order,true); assert(!gpio_is_on(gpio.gpio_state,GPIO_BUTTON_POWER));
        cleanup();
        now=1000; event(1,true); event(1,false); gpio.ap_wake_reset_pending=true;
        portable_release_all();
        assert(!ios_button_holds[1].down && !ios_button_holds[1].release->pending);
        reset_holds(order,true); assert(!gpio_is_on(gpio.gpio_state,GPIO_BUTTON_POWER));
        cleanup();
        automated_power=true; ipod_touch_press_button(IPOD_TOUCH_BUTTON_POWER,true);
        unsigned calls=sequence_cancel_calls; reset_holds(order,true);
        assert(sequence_cancel_calls==calls+1 && !automated_power);
        assert(!gpio_is_on(gpio.gpio_state,GPIO_BUTTON_POWER));
        cleanup();
    }
    now=1000; event(1,true); now=1050; reset_holds(true,true);
    assert(ios_button_holds[1].down && !ios_button_holds[1].release->pending);
    now=1500; event(1,false); assert(!ios_button_holds[1].down && !ios_button_holds[1].release->pending); cleanup();
    // Standalone cold GPIO hold clears a stale DOWN cache before later callers.
    ipod_touch_press_button(IPOD_TOUCH_BUTTON_POWER,true);
    gpio.ap_wake_reset_pending=false; s5l8900_gpio_reset((DeviceState *)&gpio);
    for(unsigned pad=0;pad<NUM_GPIO_PADS;pad++) assert(!gpio.host_input_mask[pad] && !gpio.host_input_level[pad]);
    reset_holds(true,true); assert(!gpio_is_on(gpio.gpio_state,GPIO_BUTTON_POWER)); cleanup();
    puts("PASS: production reset functions preserve only AP-wake manual timer ownership; both hold orders, cold/cancel/automation cleanup");
}
