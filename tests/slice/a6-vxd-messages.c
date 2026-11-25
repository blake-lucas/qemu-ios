/* Initialization has no decode fence: only submitted pictures complete.
 * SLICE hw/arm/s5l8920_vxd.c define VXD_BS_SEGS
 * SLICE hw/arm/s5l8920_vxd.c typedef VXDRender
 * SLICE hw/arm/s5l8920_vxd.c define MSG_|RENDER_|DEVA_RENDER_|VXD_SETUP_NS|VXD_MB_NS|VXD_PENDING
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_message
 * PKG glib-2.0
 */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#define QEMU_CLOCK_VIRTUAL 0
#define VXD_LOG(...) ((void)0)
typedef struct {
    FILE *trace;
    unsigned pending;
    uint32_t fence[16];
    bool compact[16];
    int64_t due[16], busy_until;
    void *done;
    bool a6, deva_waiver;
} S5L8920VXDState;
static bool armed;
static int64_t deadline;
static int64_t qemu_clock_get_ns(int clock)
{
    return 1000;
}
static bool timer_pending(void *timer)
{
    return armed;
}
static void timer_mod(void *timer, int64_t when)
{
    armed = true;
    deadline = when;
}
static void vxd_complete(void *opaque)
{
    assert(false);
}
static bool vxd_rw(uint32_t ptd, uint32_t addr, void *out, unsigned n,
                   bool write)
{
    return false;
}
/* The parser and codec have separate tests; here exercise message ownership. */
static bool vxd_parse_render(uint32_t ptd, uint32_t addr, void *out, bool a6)
{
    return false;
}
static bool vxd_parse_packed(uint32_t ptd, uint32_t addr, unsigned n, void *out)
{
    return false;
}
static int vxd_decode(S5L8920VXDState *s, uint32_t ptd, void *render)
{
    return 0;
}
#include "slice.h"
int main(void)
{
    uint32_t init[] = {0x8014, 0x1000, 0x101000, 0x400100, 0x12340000};
    /* AppleVXD375's boards ("a6" off) complete every message as before, INIT included (fence word 4) */
    S5L8920VXDState old = {0};
    vxd_message(&old, init, G_N_ELEMENTS(init));
    assert(old.pending == 1 && old.fence[0] == 0x12340000 && !old.compact[0]);
    armed = false;
    S5L8920VXDState s = {.a6 = true, .deva_waiver = true};
    vxd_message(&s, init, G_N_ELEMENTS(init));
    assert(!s.pending && !armed && !s.busy_until);
    uint32_t legacy_init[8] = {0x8020};
    vxd_message(&s, legacy_init, G_N_ELEMENTS(legacy_init));
    assert(!s.pending && !armed);
    uint32_t picture[] = {0x18114, 0x844015, 0x470000, 0x1234000, 0x10211000};
    vxd_message(&s, picture, G_N_ELEMENTS(picture));
    assert(s.pending == 1 && s.fence[0] == 1 && s.compact[0]);
    assert(armed && deadline == s.due[0] && deadline > 1000);
    /* Ring-wrap padding owns neither a fence nor a legacy-format reply. */
    int64_t due = s.due[0], busy_until = s.busy_until;
    uint32_t padding[] = {0xddde0004};
    vxd_message(&s, padding, G_N_ELEMENTS(padding));
    assert(s.pending == 1 && s.fence[0] == 1 && s.compact[0]);
    assert(s.due[0] == due && s.busy_until == busy_until && deadline == due);
    /* N.O.V.A.'s two-slice 480x272 picture as AppleVXD390 sends it: the first slice waives its reply (0x2000), the
     * last asks for the host interrupt (0x4000). One completion, or the driver leaves the second one unclaimed. */
    S5L8920VXDState two = {.a6 = true, .deva_waiver = true};
    uint32_t first[] = {0x18114, 0x842011, 0x2cd000, 0xbf36e000, 0x10211000};
    uint32_t last[] = {0x18114, 0x844004, 0x2cd210, 0xbf36e000, 0x10211000};
    vxd_message(&two, first, G_N_ELEMENTS(first));
    assert(!two.pending);
    vxd_message(&two, last, G_N_ELEMENTS(last));
    assert(two.pending == 1 && two.fence[0] == 1 && two.compact[0]);
    /* LT_VXD_DEVA_WAIVER=off: the old rule, a reply for each (the panic). */
    S5L8920VXDState old_rule = {.a6 = true};
    vxd_message(&old_rule, first, G_N_ELEMENTS(first));
    vxd_message(&old_rule, last, G_N_ELEMENTS(last));
    assert(old_rule.pending == 2);
    /* AppleVXD375's renders (word 7 flags) still waive with 0x0800. */
    S5L8920VXDState legacy = {.a6 = true, .deva_waiver = true};
    uint32_t slice[8] = {0x18120, 0x1234000, 0x470000, 0, 7, 0, 0, 0x0801};
    vxd_message(&legacy, slice, G_N_ELEMENTS(slice));
    assert(!legacy.pending);
    puts("ok");
}
