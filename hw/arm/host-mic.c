/*
 * The frontend's microphone: a bounded ring between the host app and an audio
 * device's capture port. See include/hw/arm/host-mic.h for the shape of the
 * path and why the phone's engine needs it (`driver=none` has no input voice).
 *
 * Rates. The app records at HOST_MIC_RATE; a port reads at its own frame rate.
 * A 16.16 accumulator picks the source frame for each output frame, nearest
 * neighbour: an exact pass-through at 44.1 kHz, and pitch-correct (aliased
 * rather than filtered) at any other rate. Linear interpolation would need the
 * ring to hold one more frame than the reader has consumed, which the
 * single-frame interface here cannot express; the guest's own HAL resamples
 * after this point anyway.
 *
 * Locking. The app pushes from its recorder thread and the port reads on the
 * vCPU thread under the BQL, so the ring has its own mutex. Neither side ever
 * waits for the other: a full ring drops the app's block, an empty one hands
 * the port silence.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"        /* stw_le_p: osdep does not pull it in (system/memory.h does) */
#include "hw/arm/host-mic.h"

#include <pthread.h>

/* The app has stopped: after this much quiet the port gets silence instead of
 * the ring's tail, so a stopped recording does not loop the last 93 ms. */
#define HOST_MIC_STALE_US 250000

static pthread_mutex_t host_mic_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t host_mic_ring[HOST_MIC_FRAMES][2];
static uint32_t host_mic_head, host_mic_level;
static unsigned host_mic_rate = HOST_MIC_RATE;
/* Source frames consumed per output frame, 16.16. */
static uint32_t host_mic_step = 1 << 16;
static uint32_t host_mic_acc;
static int64_t host_mic_last_push_us = -1;
static uint32_t host_mic_capturing;
static uint64_t host_mic_pushed, host_mic_dropped;

static void host_mic_reset_locked(void)
{
    host_mic_head = host_mic_level = 0;
    host_mic_acc = 0;
    host_mic_last_push_us = -1;
}

uint32_t host_mic_push(const void *frames, size_t bytes)
{
    const int16_t *in = frames;
    uint32_t count, taken = 0;

    if (!in || bytes < 4) {
        return 0;
    }
    count = bytes / 4;
    pthread_mutex_lock(&host_mic_lock);
    host_mic_last_push_us = g_get_monotonic_time();
    /* The guest caught up: the frame it reads next is this one, not the
     * fractional remainder of a position from before the gap. */
    if (!host_mic_level) {
        host_mic_acc = 0;
    }
    while (taken < count && host_mic_level < HOST_MIC_FRAMES) {
        uint32_t slot = (host_mic_head + host_mic_level) % HOST_MIC_FRAMES;

        host_mic_ring[slot][0] = in[2 * taken];
        host_mic_ring[slot][1] = in[2 * taken + 1];
        host_mic_level++;
        taken++;
    }
    host_mic_pushed += taken;
    host_mic_dropped += count - taken;
    pthread_mutex_unlock(&host_mic_lock);
    return taken;
}

void host_mic_flush(void)
{
    pthread_mutex_lock(&host_mic_lock);
    host_mic_reset_locked();
    host_mic_capturing = 0;
    pthread_mutex_unlock(&host_mic_lock);
}

void host_mic_set_rate(unsigned rate)
{
    if (rate < 8000 || rate > 192000) {
        rate = HOST_MIC_RATE;
    }
    pthread_mutex_lock(&host_mic_lock);
    host_mic_capturing = 1;
    if (rate != host_mic_rate) {
        host_mic_rate = rate;
        host_mic_step = (uint32_t)(((uint64_t)HOST_MIC_RATE << 16) / rate);
        if (!host_mic_step) {
            host_mic_step = 1 << 16;
        }
        host_mic_acc = 0;
    }
    pthread_mutex_unlock(&host_mic_lock);
}

bool host_mic_read_frame(uint8_t out[4])
{
    int16_t frame[2];
    bool have = false;

    pthread_mutex_lock(&host_mic_lock);
    if (host_mic_last_push_us >= 0 &&
        g_get_monotonic_time() - host_mic_last_push_us > HOST_MIC_STALE_US) {
        host_mic_reset_locked();
    }
    if (host_mic_level) {
        frame[0] = host_mic_ring[host_mic_head][0];
        frame[1] = host_mic_ring[host_mic_head][1];
        /* This frame is the one at the reader's position; the accumulator then decides how far
         * the position moves for the next one. Consuming before handing out would shift the whole
         * recording by a frame, and re-reading the head after consuming would double it. */
        host_mic_acc += host_mic_step;
        while (host_mic_acc >= (uint32_t)1 << 16 && host_mic_level) {
            host_mic_acc -= (uint32_t)1 << 16;
            host_mic_head = (host_mic_head + 1) % HOST_MIC_FRAMES;
            host_mic_level--;
        }
        have = true;
    }
    pthread_mutex_unlock(&host_mic_lock);
    if (!have) {
        return false;
    }
    stw_le_p(out, frame[0]);
    stw_le_p(out + 2, frame[1]);
    return true;
}

void host_mic_state(HostMicState *out)
{
    if (!out) {
        return;
    }
    pthread_mutex_lock(&host_mic_lock);
    out->rate = host_mic_capturing ? host_mic_rate : 0;
    out->queued = host_mic_level;
    out->capturing = host_mic_capturing;
    out->pushed = host_mic_pushed;
    out->dropped = host_mic_dropped;
    pthread_mutex_unlock(&host_mic_lock);
}
