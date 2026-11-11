/* Host button events batched by a busy emulator still have a guest-time hold, and quick clicks (a double or
 * triple click of Home) reach the guest as that many presses, each down and up long enough to see.
 *
 * Every machine's press hook (iPod touch, iPad, 1G, S5L8920, S5L8950) sees the press and the release.
 *
 * SLICE contrib/ios-app/qemu-ios-ui.c range struct ios_button { | void qemu_ios_ui_button(
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define QEMU_CLOCK_VIRTUAL 0
#define MAX(a,b) ((a)>(b)?(a):(b))
#define g_free free
#define QEMU_IOS_BUTTON_HOME 0
#define QEMU_IOS_BUTTON_POWER 1
#define QEMU_IOS_BUTTON_VOLUME_UP 2
#define QEMU_IOS_BUTTON_VOLUME_DOWN 3
typedef enum {IPOD_TOUCH_BUTTON_HOME,IPOD_TOUCH_BUTTON_POWER,IPOD_TOUCH_BUTTON_VOLUP,IPOD_TOUCH_BUTTON_VOLDOWN} IPodTouchButton;
typedef struct { void(*cb)(void*);void *arg;int64_t deadline;bool pending; } QEMUTimer;
static void ios_sequence_cancel_current(void) {}
static int64_t now;
static bool pins[4];
static int64_t qemu_clock_get_ms(int clock) { return now; }
static bool ipad_pins[4];
static void ipod_touch_press_button(IPodTouchButton b,bool down) {pins[b]=down;}
static void ipad1_press_button(IPodTouchButton b,bool down) {ipad_pins[b]=down;}
static char g1_pins[8];
static void ipod_touch_1g_press_button(IPodTouchButton b,bool down) {g1_pins[b]=down;}
static bool s5l8920_pins[4];
static void s5l8920_press_button(IPodTouchButton b,bool down) {s5l8920_pins[b]=down;}
static bool s5l8950_pins[4];
static void s5l8950_press_button(IPodTouchButton b,bool down) {s5l8950_pins[b]=down;}
static QEMUTimer *timer_new_ms(int c,void(*cb)(void*),void *arg) {
 QEMUTimer *t=calloc(1,sizeof(*t));t->cb=cb;t->arg=arg;return t;
}
static void timer_del(QEMUTimer*t) { t->pending=false; }
static void timer_mod(QEMUTimer*t,int64_t d) {t->deadline=d;t->pending=true;}
#include "slice.h"
static void event(int button,bool down) {
 struct ios_button *b=calloc(1,sizeof(*b));b->button=button;b->down=down;ios_button_bh(b);
}
/* Fire the button's timer while it is due by `t`. */
static void advance(int b,int64_t t) {
 QEMUTimer*timer=ios_button_holds[b].release;
 while(timer && timer->pending && timer->deadline<=t) { now=timer->deadline; timer->pending=false; timer->cb(timer->arg); }
 now=t;
}
/* Run Home up to `t` at 1 ms steps, counting the presses (rising edges) the guest sees. */
static int presses;
static void run_to(int64_t t) {
 for(int64_t ms=now+1;ms<=t;ms++) { bool was=pins[0]; advance(0,ms); presses+=pins[0]&&!was; }
}
/* A host event on Home at `t`. */
static void at(int64_t t,bool down) { run_to(t); bool was=pins[0]; event(0,down); presses+=pins[0]&&!was; }
static void reset(void) {
 for(int b=0;b<4;b++) { free(ios_button_holds[b].release); memset(&ios_button_holds[b],0,sizeof(ios_button_holds[b])); }
 memset(pins,0,sizeof pins);
}
int main(void) {
 for(int b=0;b<4;b++) {
  now=1000;event(b,true);event(b,false);
  QEMUTimer*t=ios_button_holds[b].release;
  assert(pins[b] && ipad_pins[b] && g1_pins[b] && s5l8920_pins[b] && s5l8950_pins[b] && t->pending && t->deadline==1100);
  now=1100;t->cb(t->arg);assert(!pins[b] && !ipad_pins[b] && !g1_pins[b] && !s5l8920_pins[b] && !s5l8950_pins[b] && !t->pending);
  // A long hold passes through as the host made it.
  now=2000;event(b,true);assert(pins[b] && !t->pending);now=2500;event(b,false);assert(!pins[b] && !t->pending);
  // A second click inside the first's hold is a second press, not one long one.
  now=3000;event(b,true);event(b,false);now=3050;event(b,true);event(b,false);
  assert(pins[b] && t->deadline==3100);
  advance(b,3100);assert(!pins[b] && t->deadline==3200);
  advance(b,3200);assert(pins[b] && t->deadline==3300);
  advance(b,3300);assert(!pins[b] && !t->pending && ios_button_holds[b].queued==0);
 }
 reset();
 // A triple click of Home faster than the guest takes it: three presses, then up.
 reset();presses=0;now=10000;
 for(int i=0;i<3;i++) { at(10000+40*i,true); at(10000+40*i+20,false); }
 run_to(11000); assert(presses==3 && !pins[0] && !ios_button_holds[0].release->pending);
 // Eight quick clicks: the one applied at once and three waiting; the rest are dropped whole, never stuck down.
 reset();presses=0;now=20000;
 for(int i=0;i<8;i++) { at(20000+10*i,true); at(20000+10*i+5,false); }
 run_to(22000); assert(presses==4 && !pins[0]);
 // A repeated down (a key's auto-repeat) is one press, released by the next up.
 reset();presses=0;now=30000;at(30000,true);at(30010,true);at(30500,false);run_to(31000);
 assert(presses==1 && !pins[0] && !ios_button_holds[0].queued);
 // Clicks spaced wider than a press pass through untouched.
 reset();presses=0;now=40000;
 for(int i=0;i<3;i++) { at(40000+300*i,true); assert(pins[0]); at(40000+300*i+150,false); assert(!pins[0]); }
 assert(presses==3);
 reset();
 event(-1,true);event(4,true);
 puts("PASS: batched button events, long holds, double and triple clicks, the waiting limit, repeats, invalid buttons");
}
