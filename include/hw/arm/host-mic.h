/*
 * The frontend's microphone, as an audio device's capture port sees it.
 *
 * The engine on a phone runs its audio backend as `driver=none`, so a capture
 * port's input voice never opens and a recording app in the guest hears
 * silence. This is the input counterpart of the output capture tap the portable
 * library installs: the frontend pushes the frames its microphone recorded and
 * a port hands them to the guest as if an input voice had queued them.
 *
 *   host app -> host_mic_push() -> ring -> host_mic_read_frame() -> RX FIFO
 *
 * The capture port asks this ring BEFORE its own input voice: the phone's
 * engine has no real input voice, but `driver=none`'s silence-producing one
 * opens anyway (audio/noaudio.c), so a fallback behind it would never run.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HOST_MIC_H
#define HW_ARM_HOST_MIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The rate the frontend records at: what iOS uses, and what AudioRecord opens
 * at on every Android device. A capture port at another rate is resampled. */
#define HOST_MIC_RATE 44100

/* Frames of stereo S16 the ring holds: 93 ms at 44.1 kHz, enough to cover the
 * app's block (23 ms) and a burst on the guest side, and small enough that a
 * guest which stops reading costs a fraction of a second of audio. */
#define HOST_MIC_FRAMES 4096

typedef struct HostMicState {
    uint32_t rate;          /* the capture port's rate; 0 before one starts */
    uint32_t queued;        /* frames waiting for the guest */
    uint32_t capturing;     /* a port has asked for input since the last flush */
    uint64_t pushed;        /* frames the frontend handed over */
    uint64_t dropped;       /* frames the ring had no room for */
} HostMicState;

/* Frames the frontend recorded: interleaved stereo S16, HOST_MIC_RATE, `bytes`
 * a multiple of 4. Returns the frames accepted; the rest did not fit. */
uint32_t host_mic_push(const void *frames, size_t bytes);
/* Forget everything: a port started, stopped, or the app stopped recording. */
void host_mic_flush(void);
/* The rate the capture port reads at, from its own configuration. */
void host_mic_set_rate(unsigned rate);
/* One stereo S16 frame at the port's rate, little-endian, as the RX FIFO holds
 * it. False when the app has nothing for the guest (which then reads silence). */
bool host_mic_read_frame(uint8_t out[4]);
void host_mic_state(HostMicState *out);

#endif
