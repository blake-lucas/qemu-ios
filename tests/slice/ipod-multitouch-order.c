/* Serially consume production Z2 reports; model dispatch is not guest receipt.
 * SLICE:wire include/hw/arm/ipod_touch_multitouch.h range #define MT_INTERFACE_VERSION | typedef struct IPodTouchMultitouchState
 * SLICE:state include/hw/arm/ipod_touch_multitouch.h typedef IPodTouchMultitouchState
 * SLICE:profile hw/arm/ipod_touch_multitouch.c range const MTSensorProfile mt_profile_ipod = { | const MTSensorProfile mt_profile_k48
 * SLICE hw/arm/ipod_touch_multitouch.c fn ipod_touch_multitouch_can_queue mt_clear_pending mt_take_pending mt_clamp_vel mt_frame_slots mt_const_fingerid mt_build_frame mt_any_down mt_any_active mt_emit_frame mt_cancel_pending_end mt_frame_period_ns ipod_touch_multitouch_set_finger
 * SLICE:transfer hw/arm/ipod_touch_multitouch.c fn mt_stamp_frame_cmd ipod_touch_multitouch_transfer
 * SLICE:lifecycle hw/arm/ipod_touch_multitouch.c fn touch_timer_tick touch_end_timer_tick ipod_touch_multitouch_reset
 * SLICE:finalize hw/arm/ipod_touch_multitouch.c fn ipod_touch_multitouch_finalize
 * VARIANT
 * VARIANT -D__APPLE__
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define MT_BUFFER_SIZE 0x400
#define g_malloc0(n) calloc(1,n)
#define g_free free
#define QEMU_CLOCK_VIRTUAL 0
#define NANOSECONDS_PER_SECOND 1000000000LL
#define MTT(...) ((void)0)
typedef struct {} SSIPeripheral;
typedef struct {} DeviceState;
typedef struct {} Object;
typedef struct {} QEMUTimer;
typedef struct {} IPodTouchSYSICState;
typedef struct {} IPodTouchGPIOState;
typedef int qemu_irq;
#define qemu_irq_raise(irq) ((void)(irq))   /* the A6's hold-atn, off here */
static uint64_t now = 1000000000;
static uint64_t qemu_clock_get_ns(int clock) { return now; }
static void timer_del(QEMUTimer *t) {}
static void timer_mod(QEMUTimer *t, int64_t deadline) {}
static void timer_free(QEMUTimer *t) { free(t); }
static bool mt_trace(void) { return false; }
#include "wire.h"
#include "state.h"
#include "profile.h"
#define IPOD_TOUCH_MULTITOUCH(s) ((IPodTouchMultitouchState *)(s))
static unsigned notifications;
static void ipod_touch_multitouch_inform_frame_ready(IPodTouchMultitouchState *s) { notifications++; }
#include "slice.h"
/* Other commands are not used by this serial frame test. */
static void prepare_interface_version_response(IPodTouchMultitouchState *s) { abort(); }
static void prepare_report_info_response(IPodTouchMultitouchState *s, uint8_t r) { abort(); }
static void prepare_short_control_response(IPodTouchMultitouchState *s, uint8_t r) { abort(); }
static void mt_n1_read(IPodTouchMultitouchState *s) { abort(); }
static void mt_n1_select(IPodTouchMultitouchState *s, uint8_t w) { abort(); }
#define MT_HBPP_MAX_DATA_LEN (((0xffu << 10) | (0xffu << 2)) + 5)
static void prepare_cmd_status_response(IPodTouchMultitouchState *s) { abort(); }
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
#define IT_IDX(n, i, size) (assert((i) < (size)), (i))
#define IT_SIZE(n, i, size) (assert((i) <= (size)), (i))
#include "transfer.h"
#include "lifecycle.h"
#if !defined(__APPLE__)
#include "finalize.h"
#endif
static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned read_frame(IPodTouchMultitouchState *s, uint8_t p[MT_BUFFER_SIZE])
{
    p[0] = ipod_touch_multitouch_transfer(&s->ssidev, MT_CMD_FRAME_READ_V2);
    for (unsigned i = 1; i < 16; i++)
        p[i] = ipod_touch_multitouch_transfer(&s->ssidev, 0xff);
    unsigned length = 21 + le16(p + 1), sum = 0;
    assert(length <= MT_BUFFER_SIZE && length >= 75);
    for (unsigned i = 16; i < length; i++)
        p[i] = ipod_touch_multitouch_transfer(&s->ssidev, 0xff);
    for (unsigned i = 0; i < 14; i++) sum += p[i];
    assert((sum & 65535) == le16(p + 14));
    sum = 0;
    for (unsigned i = 21; i < length - 2; i++) sum += p[i];
    assert((sum & 65535) == le16(p + length - 2));
    assert(s->cur_cmd == 0);
    return p[37]; /* frame header numFingers */
}
int main(void)
{
    IPodTouchMultitouchState s = {.profile = &mt_profile_ipod};
    uint8_t p[MT_BUFFER_SIZE];
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, true);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    now += 80000000;
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, false); /* cancel/END */
    ipod_touch_multitouch_set_finger(&s, 0, .8f, .5f, true);  /* no intervening SPI read */
#if defined(__APPLE__)
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    mt_clear_pending(&s); free(s.out_buffer); free(s.in_buffer);
    puts("PASS: original Apple single pending report behavior preserved");
#else
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_ENDED);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    ipod_touch_multitouch_reset((DeviceState *)&s);
    /* The initial START, END and fresh START all precede the first read. */
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, true);
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, false);
    ipod_touch_multitouch_set_finger(&s, 0, .8f, .5f, true);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_ENDED);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    ipod_touch_multitouch_reset((DeviceState *)&s);
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, true);
    for (unsigned i = 0; i < 10; i++) {
        now += 20000000;
        ipod_touch_multitouch_set_finger(&s, 0, .3f + i * .05f, .5f, true);
    }
    assert(s.queued_count == 1); /* START plus latest MOVED */
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_MOVED);
    assert((int16_t)le16(p + 49) == -74 + (int)(.75f * 4720));
    ipod_touch_multitouch_reset((DeviceState *)&s);
    while (ipod_touch_multitouch_can_queue(&s, 1, 0, false)) {
        ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
        ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, false);
    }
    assert(s.queued_count < MT_REPORT_QUEUE_SIZE);
    unsigned queued = s.queued_count;
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
    assert(s.queued_count == queued && s.fingers[0].phase != MT_FINGER_DOWN);
    /* The preserved frames still alternate; saturation cannot overwrite END. */
    for (unsigned i = 0; i <= queued; i++) {
        assert(read_frame(&s, p) == 1);
        assert(p[46] == (i % 2 ? MT_EVENT_TOUCH_ENDED : MT_EVENT_TOUCH_START));
    }
    ipod_touch_multitouch_reset((DeviceState *)&s);
    assert(!s.next_frame && !s.queued_count);
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, true);
    ipod_touch_multitouch_set_finger(&s, 0, .5f, .5f, false);
    touch_end_timer_tick(&s); /* retain FULL_END too */
    unsigned notified = notifications;
    touch_timer_tick(&s);
    assert(notifications > notified); /* pending reports keep notifying after lift */
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_ENDED);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_FULL_END);
    assert(read_frame(&s, p) == 0);
    /* Multi-contact saturation still reserves cancellation for live slots. */
    ipod_touch_multitouch_reset((DeviceState *)&s);
    for (unsigned i = 0; i < MT_MAX_FINGERS; i++)
        ipod_touch_multitouch_set_finger(&s, i, .1f + .15f * i, .5f, true);
    for (unsigned i = 0; i < MT_MAX_FINGERS; i++) read_frame(&s, p);
    ipod_touch_multitouch_set_finger(&s, 0, .1f, .5f, false);
    while (ipod_touch_multitouch_can_queue(&s, 1, 0, false)) {
        ipod_touch_multitouch_set_finger(&s, 0, .1f, .5f, true);
        ipod_touch_multitouch_set_finger(&s, 0, .1f, .5f, false);
    }
    assert(ipod_touch_multitouch_can_queue(&s, 0, 4, false));
    for (unsigned i = 1; i < MT_MAX_FINGERS; i++)
        ipod_touch_multitouch_set_finger(&s, i, .1f + .15f * i, .5f, false);
    touch_end_timer_tick(&s);
    assert(s.queued_count + !!s.next_frame <= MT_REPORT_QUEUE_SIZE);
    assert(!ipod_touch_multitouch_can_queue(&s, 1, 0, false));
    assert(!mt_any_down(&s));
    if (s.queued_count + !!s.next_frame == MT_REPORT_QUEUE_SIZE) {
        read_frame(&s, p); /* one spare slot; mixed motion + END needs two */
        assert(ipod_touch_multitouch_can_queue(&s, 0, 1, false));
        assert(!ipod_touch_multitouch_can_queue(&s, 0, 1, true));
    }
    /* Reset frees every retained report; no canceled gesture survives it. */
    ipod_touch_multitouch_reset((DeviceState *)&s);
    assert(!s.next_frame && !s.queued_count && !mt_any_active(&s));
    ipod_touch_multitouch_set_finger(&s, 0, .7f, .5f, true);
    assert(read_frame(&s, p) == 1 && p[46] == MT_EVENT_TOUCH_START);
    assert((int16_t)le16(p + 49) == -74 + (int)(.7f * 4720));
    s.n1_frame = calloc(1, 75);
    s.touch_timer = calloc(1, 1);
    s.touch_end_timer = calloc(1, 1);
    ipod_touch_multitouch_finalize((Object *)&s);
    puts("PASS: serial SPI ordered START/END/reuse, latest motion, bounded saturation, reset/free and release notification");
#endif
}
