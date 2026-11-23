/* Production SPI handover echoes its read command without altering report/contact bytes.
 * Prefix/body integrity and FIFO ownership are host-model checks, not guest acceptance.
 * SLICE:wire include/hw/arm/ipod_touch_multitouch.h range #define MT_INTERFACE_VERSION | typedef struct IPodTouchMultitouchState
 * SLICE:state include/hw/arm/ipod_touch_multitouch.h typedef IPodTouchMultitouchState
 * SLICE:profile hw/arm/ipod_touch_multitouch.c range const MTSensorProfile mt_profile_ipod = { | const MTSensorProfile mt_profile_k48
 * SLICE hw/arm/ipod_touch_multitouch.c fn mt_stamp_frame_cmd ipod_touch_multitouch_can_queue mt_clear_pending mt_take_pending mt_clamp_vel mt_frame_slots mt_const_fingerid mt_build_frame mt_any_down mt_any_active mt_emit_frame mt_cancel_pending_end mt_frame_period_ns ipod_touch_multitouch_set_finger
 * SLICE:transfer hw/arm/ipod_touch_multitouch.c fn ipod_touch_multitouch_transfer
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
static unsigned read_cmd(IPodTouchMultitouchState *s, uint8_t *p, uint8_t cmd)
{
    p[0] = ipod_touch_multitouch_transfer(&s->ssidev, cmd);
    for (unsigned i = 1; i < 16; i++)
        p[i] = ipod_touch_multitouch_transfer(&s->ssidev, 0xff);
    unsigned length = 21 + le16(p + 1), sum = 0;
    assert(length >= 75 && length <= MT_BUFFER_SIZE);
    for (unsigned i = 16; i < length; i++)
        p[i] = ipod_touch_multitouch_transfer(&s->ssidev, 0xff);
    assert(p[0] == cmd && p[16] == cmd);
    for (unsigned i = 0; i < 14; i++) sum += p[i];
    assert((sum & 65535) == le16(p + 14));
    sum = 0;
    for (unsigned i = 16; i < 21; i++) sum += p[i];
    assert((sum & 255) == 0 && le16(p + 18) == le16(p + 1));
    sum = 0;
    for (unsigned i = 21; i < length - 2; i++) sum += p[i];
    assert((sum & 65535) == le16(p + length - 2));
    assert(s->cur_cmd == 0);
    return length;
}
/* Snapshot a production-built queued report, then clock the actual production
 * transfer handler. Only cmd/checksum prefix bytes may change at handover. */
static unsigned preserved(IPodTouchMultitouchState *s, uint8_t *p, uint8_t cmd)
{
    assert(s->next_frame);
    unsigned before_len = s->next_frame_len;
    uint8_t before[MT_BUFFER_SIZE];
    assert(before_len <= sizeof(before));
    memcpy(before, s->next_frame, before_len);
    unsigned length = read_cmd(s, p, cmd);
    assert(length == before_len);
    assert(!memcmp(before + 21, p + 21, length - 21));
    for (unsigned i = 0; i < 21; i++) {
        if (i != 0 && i != 14 && i != 15 && i != 16 && i != 20)
            assert(before[i] == p[i]);
    }
    return length;
}
static void finish(IPodTouchMultitouchState *s)
{
    mt_clear_pending(s);
    free(s->out_buffer);
    free(s->in_buffer);
    s->out_buffer = s->in_buffer = NULL;
}
int main(void)
{
    unsetenv("IT_MT_PAD_FINGERS"); unsetenv("IT_MT_FINGERID");
    unsetenv("IT_MT_EMPTY_TEST");
    uint8_t p[MT_BUFFER_SIZE];
    /* Real builder + real take_pending/transfer path, for all required counts. */
    unsigned counts[] = {0, 1, 2, 5};
    for (unsigned c = 0; c < sizeof(counts)/sizeof(counts[0]); c++) {
        for (unsigned v = 0; v < 2; v++) {
            IPodTouchMultitouchState s = {.profile = &mt_profile_ipod};
            MTFingerState fingers[MT_MAX_FINGERS] = {0};
            for (unsigned i = 0; i < counts[c]; i++)
                fingers[i] = (MTFingerState){.phase=MT_FINGER_DOWN,
                    .x=.1f+.15f*i, .y=.2f+.1f*i};
            now += 100000000;
            s.next_frame = mt_build_frame(&s, counts[c] ? fingers : NULL,
                                          &s.next_frame_len);
            /* A queued frame already carrying EB can be read with EA too;
             * repeated stamping must not double-adjust either checksum. */
            mt_stamp_frame_cmd(s.next_frame, 0xeb);
            mt_stamp_frame_cmd(s.next_frame, 0xeb);
            preserved(&s, p, v ? 0xeb : 0xea);
            assert(p[37] == counts[c] && !s.next_frame);
            for (unsigned i = 0; i < counts[c]; i++) {
                assert(p[45+28*i] == i+1);
                assert(p[46+28*i] == MT_EVENT_TOUCH_START);
            }
            uint64_t timestamp = s.last_frame_timestamp;
            /* No-pending polls must return a fresh well-formed empty report,
             * echo each command, and preserve the velocity time base. */
            for (unsigned k = 0; k < 3; k++) {
                now += 1000000;
                unsigned length = read_cmd(&s, p, k == 1 ? 0xeb : 0xea);
                assert(length == 75 && p[37] == 0 && !s.next_frame);
                for (unsigned i = 45; i < 73; i++) assert(!p[i]);
                assert(s.last_frame_timestamp == timestamp);
            }
            finish(&s);
        }
    }
#if !defined(__APPLE__)
    IPodTouchMultitouchState s = {.profile = &mt_profile_ipod};
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, true);
    now += 80000000;
    ipod_touch_multitouch_set_finger(&s, 0, .2f, .5f, false);
    ipod_touch_multitouch_set_finger(&s, 0, .8f, .5f, true);
    assert(s.queued_count == 2);
    preserved(&s, p, 0xeb); assert(p[45] == 1 && p[46] == MT_EVENT_TOUCH_START);
    preserved(&s, p, 0xea); assert(p[45] == 1 && p[46] == MT_EVENT_TOUCH_ENDED);
    preserved(&s, p, 0xeb); assert(p[45] == 1 && p[46] == MT_EVENT_TOUCH_START);
    assert(!s.next_frame && !s.queued_count);
    ipod_touch_multitouch_set_finger(&s, 0, .8f, .5f, false);
    touch_end_timer_tick(&s);
    preserved(&s, p, 0xea); assert(p[46] == MT_EVENT_TOUCH_ENDED);
    preserved(&s, p, 0xeb); assert(p[46] == MT_EVENT_TOUCH_FULL_END);
    assert(!s.next_frame && !s.queued_count);
    finish(&s);
#endif
    puts("PASS: production queued/empty SPI handover echoes EA/EB; prefix/body sums, contact bytes, IDs, release ordering and idle timestamp preserved");
    return 0;
}
