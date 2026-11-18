/*
 * Guards for guest-supplied values that reach a host array index, a host
 * length, or a register field.
 *
 * This tree has a recurring bug shape: a value the guest wrote into a register
 * (or left in a descriptor in guest memory) is used directly as an index into a
 * host array, as a memcpy/malloc length, or shifted into a status field with no
 * mask. It produced the multitouch heap overrun, and a one-shot audit found six
 * more instances. The damage never surfaces where it is caused -- it lands in
 * whatever allocated next, as a monitor assert, a TCG segfault or an allocator
 * abort. See the project note `qemu-ios-heap-corruption-tell`.
 *
 * The rule these exist to make enforceable: no guest-supplied value reaches a
 * `[]`, a `malloc`, or a length argument without passing through one of these.
 *
 *   IT_IDX(dev, idx, n)     bound an array index to [0, n), clamping high
 *   IT_SIZE(dev, n, max)    bound a length or allocation to [0, max]
 *   IT_RANGE(dev, off, len, size)  true when [off, off + len) lies in [0, size),
 *                           computed without overflow; refuse the operation if not
 *   IT_FIELD(v, width)      mask a value into a `width`-bit register field
 *
 * Clamping (IT_IDX, IT_SIZE) suits a value whose out-of-range use only has to stay
 * inside the array; where acting on a clamped value would be wrong (a write to the
 * wrong register, a short copy reported as complete), test with IT_RANGE and refuse.
 * Every guest-driven host allocation also has a per-device cap (IT_SIZE against a
 * named constant, or a refusal).  tests/lint/guest-bounds.py, run by
 * tests/gate.sh --quick, flags guest values reaching an index, a length or an
 * allocation without one of these or an explicit bounds test.
 *
 * IT_IDX, IT_SIZE and IT_RANGE log once per occurrence at LOG_GUEST_ERROR with the device
 * name and the offending expression, then return a safe value -- they never
 * abort. IT_FIELD is silent: truncating to the field width is what the hardware
 * does, not an error.
 *
 * `dev` is a short literal device name for the log ("pl192", "multitouch").
 */

#ifndef HW_ARM_IPOD_TOUCH_GUARD_H
#define HW_ARM_IPOD_TOUCH_GUARD_H

#include "qemu/osdep.h"
#include "qemu/log.h"

static inline uint64_t it_guard_idx(const char *dev, const char *expr,
                                    uint64_t idx, uint64_t n)
{
    if (n == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "[%s] %s: index bound is zero, no valid index exists\n",
                      dev, expr);
        return 0;
    }
    if (idx >= n) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "[%s] %s = %" PRIu64 " is out of range (limit %" PRIu64
                      "); clamped\n", dev, expr, idx, n);
        return n - 1;
    }
    return idx;
}

static inline uint64_t it_guard_size(const char *dev, const char *expr,
                                     uint64_t n, uint64_t max)
{
    if (n > max) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "[%s] %s = %" PRIu64 " exceeds the maximum %" PRIu64
                      "; truncated\n", dev, expr, n, max);
        return max;
    }
    return n;
}

static inline bool it_guard_range(const char *dev, const char *expr,
                                  uint64_t off, uint64_t len, uint64_t size)
{
    if (off > size || len > size - off) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "[%s] %s: 0x%" PRIx64 " + 0x%" PRIx64 " is outside 0x%"
                      PRIx64 "; refused\n", dev, expr, off, len, size);
        return false;
    }
    return true;
}

#define IT_IDX(dev, idx, n)    it_guard_idx((dev), #idx, (uint64_t)(idx), \
                                            (uint64_t)(n))
#define IT_SIZE(dev, n, max)   it_guard_size((dev), #n, (uint64_t)(n), \
                                             (uint64_t)(max))
#define IT_RANGE(dev, off, len, size) \
    it_guard_range((dev), #off " + " #len, (uint64_t)(off), (uint64_t)(len), \
                   (uint64_t)(size))
#define IT_FIELD(v, width)     ((uint32_t)((v) & ((1u << (width)) - 1)))

#endif /* HW_ARM_IPOD_TOUCH_GUARD_H */
