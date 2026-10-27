/*
 * The bridge between this emulator and a host iOS app: frames out, touches in.
 *
 * Deliberately NOT a `-display ios` backend. A registered display type has to
 * be declared in qapi/ui.json and selected on the command line, and none of
 * that buys anything here -- the app is in the same process and can simply
 * call these functions. So the VM runs with `-display none` and the app
 * attaches afterwards.
 *
 * THREADING is the whole difficulty. Everything below marked "QEMU thread"
 * runs under the BQL on the thread executing qemu_ios_main(); everything
 * marked "app thread" is called from UIKit. The two never touch the same
 * state without going through a bottom half, because QEMU's console and input
 * layers assume the BQL is held and UIKit assumes the main thread.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/seqlock.h"
#include "qemu/cutils.h"
#define IOS_FRAME_PATH_DEFINES_EVENT /* strong definitions on Windows: see the header */
#include "qemu/ios-frame-path.h"
#include "hw/arm/ipod_touch_lis302dl.h"
#include "hw/arm/nand-image.h"
#include "qemu/aio.h"
#include "system/runstate.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "qapi/error.h"
#include "hw/arm/ipod_touch_buttons.h"
#if !defined(__APPLE__)
#include "hw/arm/ipod_touch_multitouch.h"
#endif
#include "hw/core/boards.h"

void gles_host_set_allowed(bool allowed);
bool ipod_touch_fmss_io_failed(void);
bool ipod_touch_nor_io_failed(void);
bool s5l8930_iop_io_failed(void);
bool ipod_touch_mipi_dsi_panel_off(void);
bool s5l8930_d1815_guest_shutdown_confirmed(void);

#include "qemu-ios-ui.h"
#include "virtual-input.h"
#include "qemu-ios-input.h"
#include "qemu-ios-input.c.inc"

static void ios_sequence_cancel_current(void);
static bool ios_manual_touch;
static bool ios_manual_touch2;
static bool portable_has_gyro;
static int portable_paused;
static void portable_runstate_changed(void *opaque, bool running, RunState state)
{
    qatomic_set(&portable_paused, !running);
}
static void portable_release_all(void);
static void portable_release_input(bool preserve_buttons);
#ifndef __APPLE__
static void ios_cancel_publish(bool stopping);
#endif
static void ios_sequence_reset(void *opaque)
{
    ios_sequence_cancel_current();
#ifndef __APPLE__
    ios_cancel_publish(false);
    portable_release_input(ipod_touch_ap_wake_reset_pending());
#else
    portable_release_all();
#endif
}
#include "hw/arm/ipod_touch_pcf50633_pmu.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "hw/arm/guest-services/gles.h"

#ifndef _WIN32
#include <sys/resource.h>
#include <dlfcn.h>
#endif

#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "migration/misc.h"
#include "migration/migration.h"      /* migrate_get_current, MigrationState.state */
#include "system/runstate.h"
#include "system/reset.h"

#define IOS_MAX_SLOTS 8

#include "qemu-ios-frame-path.c.inc"

/*
 * Frames rotate through a ring rather than a single buffer so the app can wrap
 * the published one in a CGImage and hand it to the compositor WITHOUT copying
 * it: by the time the emulator writes this buffer again, IOS_FRAME_BUFFERS
 * frames have gone by and the image is long since drawn. One buffer would mean
 * either a copy per frame or the guest repainting pixels that are on screen.
 */
#define IOS_FRAME_BUFFERS 3

/* Set once the console/display are attached, cleared when the main loop returns. */
static int ios_vm_alive;
/* Which machine is running, latched on the QEMU thread when the VM starts. */
static int ios_is_ipad1;
static int ios_dsi_panel;       /* the panel is powered over DSI: the iPad's and the S5L8920 boards' */

static struct {
    DisplayChangeListener dcl;
    QemuConsole *con;
    bool attached;

    /* Written on the QEMU thread, read by the app thread under frame_lock. */
    QemuMutex frame_lock;
    void *buf[IOS_FRAME_BUFFERS];
    size_t buf_size;          /* capacity of each, never shrinks */
    GSList *retired;          /* buffers a CGImage may still be reading */
    int published;            /* index of the newest complete frame, -1 if none */
    int width, height;
    uint64_t serial;          /* bumped on every completed frame */
    uint64_t timestamp_ns;

    qemu_ios_frame_cb cb;
    void *cb_opaque;

    struct touch_slot slots[INPUT_EVENT_SLOTS_MAX];

    /* Set by a partial update, cleared when a whole frame is published. */
    bool pending;
#ifndef __APPLE__
    /* Main AioContext/BQL only; never retains a DisplaySurface pointer. */
    QEMUTimer *publish_timer;
#endif
} ios;

/* Protected by frame_lock, including getters; not reset or migrated.
 * Valid copy calls partition into no-frame, metadata-only, buffer-small and
 * actual copies. Metadata queries never count as copying guest pixels. */
typedef struct IosFrameCounters {
    uint64_t capture_attempts, identical_suppressed, publications;
    uint64_t valid_copy_calls, no_frame, metadata_only, buffer_small, pixel_copies;
    uint64_t last_copied_serial;
} IosFrameCounters;
static IosFrameCounters ios_frame_counters;

static void ios_frame_count(uint64_t *counter)
{
    if (*counter != UINT64_MAX) {
        ++*counter;
    }
}

/* Frame polling can begin even when startup fails before attach(). Initialize
 * on every entry path through GLib's once primitive, including concurrent first
 * reads. The lock lives for the process; published buffers outlive VM exit. */
static void ios_init_frame_lock(void)
{
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        qemu_mutex_init(&ios.frame_lock);
        ios.published = -1;
        g_once_init_leave(&initialized, 1);
    }
}

/* Called by QOM under the BQL, then takes frame_lock in that order.
 * Allocation occurs only for explicit queries, never per frame. */
static char *ios_get_portable_frame_counters(Object *obj, Error **errp)
{
    char *result;
    const IosFrameCounters *c = &ios_frame_counters;

    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);
    result = g_strdup_printf("{\"schema\":1,\"captureAttempts\":%" PRIu64
        ",\"identicalSuppressed\":%" PRIu64 ",\"publications\":%" PRIu64
        ",\"publishedSerial\":%" PRIu64 ",\"validCopyCalls\":%" PRIu64
        ",\"noFrame\":%" PRIu64 ",\"metadataOnly\":%" PRIu64
        ",\"bufferSmall\":%" PRIu64 ",\"pixelCopies\":%" PRIu64
        ",\"lastCopiedSerial\":%" PRIu64
        ",\"publishedBuffer\":%d,\"pending\":%s,\"atomicWithLcd\":false}",
        c->capture_attempts, c->identical_suppressed, c->publications,
        ios.serial, c->valid_copy_calls, c->no_frame, c->metadata_only,
        c->buffer_small, c->pixel_copies, c->last_copied_serial,
        ios.published, ios.pending ? "true" : "false");
    qemu_mutex_unlock(&ios.frame_lock);
    return result;
}

static void ios_add_frame_counter_property(void)
{
    /* The normal headless LCD has no dependency on this optional UI bridge. */
    Object *lcd = object_resolve_path_type("", TYPE_IPOD_TOUCH_LCD, NULL);

    if (lcd && !object_property_find(lcd, "portable-frame-counters")) {
        object_property_add_str(lcd, "portable-frame-counters",
                                ios_get_portable_frame_counters, NULL);
        object_property_set_description(lcd, "portable-frame-counters",
            "Read-only process-lifetime saturating capture/publication/copy totals; "
            "validCopyCalls excludes invalid ABI calls; metadata-only is not a pixel copy");
    }
}

/* --- frames out: QEMU thread ------------------------------------------- */

/*
 * Time to the first frame the guest has actually DRAWN.
 *
 * "First frame" on its own is worthless as a boot measurement: the panel
 * publishes a blank surface the moment the console exists, which is a few
 * milliseconds in and says nothing about the guest. The first frame with
 * light in it is the earliest honest sign the guest is alive, and it is the
 * only figure comparable between TCG backends.
 *
 * Sampled sparsely and only until it fires, so it costs nothing thereafter.
 */
static void ios_report_first_lit_frame(const uint8_t *bgra, int w, int h)
{
    static bool reported;
    static int64_t first_capture_ns;
    size_t pixels, lit = 0, i;

    if (reported) {
        return;
    }
    if (first_capture_ns == 0) {
        first_capture_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }

    pixels = (size_t)w * h;
    for (i = 0; i < pixels; i += 64) {
        const uint8_t *p = bgra + i * 4;
        if (p[0] > 24 || p[1] > 24 || p[2] > 24) {
            lit++;
        }
    }

    /* A handful of stray pixels is noise; a drawn screen is nothing like it. */
    if (lit * 64 > pixels / 100) {
        double cpu = -1;

        reported = true;

        /*
         * CPU time as well as wall time, because wall time cannot compare TCG
         * backends on this workload: boot is mostly fixed timer and I/O waits,
         * so a backend that burns half the cycles still finishes at about the
         * same moment. CPU seconds for identical guest work is the honest
         * measure of interpreter throughput.
         */
#ifdef _WIN32
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            ULARGE_INTEGER system_time, user_time;
            system_time.LowPart = kernel.dwLowDateTime;
            system_time.HighPart = kernel.dwHighDateTime;
            user_time.LowPart = user.dwLowDateTime;
            user_time.HighPart = user.dwHighDateTime;
            cpu = (system_time.QuadPart + user_time.QuadPart) / 1e7;
        }
#else
        struct rusage ru;
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
                + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
        }
#endif

        double wall = (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - first_capture_ns) / 1e9;
        if (cpu >= 0) {
            fprintf(stderr, "[boot] first lit frame after %.1f s wall, %.1f s cpu\n", wall, cpu);
        } else {
            fprintf(stderr, "[boot] first lit frame after %.1f s wall, cpu unavailable\n", wall);
        }
    }
}

/*
 * Perf work: IT_FRAME_TRACE=path appends one line per published frame --
 * "ns serial vcpu-run vcpu-outside bql-outside bql-inside vcpu-cpu insns
 * gl-call gl-present main-cpu" (see it_acct_frame_snapshot), all cumulative
 * ns except insns -- so that a slow frame can be
 * put down to guest work, guest idle, the BQL, GL or the host scheduler
 * (tools/perf/framestats.py --trace). The vCPU figures need IT_VCPU_ACCT,
 * the GL ones IT_GLES_PROF.
 */
void it_acct_frame_snapshot(int64_t v[6]);
void it_acct_vcpu_sched(int64_t v[3]);
/* Capture producers only format and enqueue bounded records. The writer never
 * holds this mutex during file I/O, and this mutex never acquires the BQL.
 * Completed GL calls are grouped by publication interval; S rows retain slow
 * call intervals, including calls spanning a publication boundary. */
extern bool it_profile_on;
void it_profile_cb_enable(void);
static GMutex profile_mutex;
static FILE *profile_file;
static int profile_error;
#define PROFILE_QUEUE_BYTES (8 * 1024 * 1024)
static GCond profile_cond;
static GThread *profile_thread;
static char *profile_queue;
static size_t profile_head, profile_queued;
static uint64_t profile_bytes;
static bool profile_closing, profile_sync;

/* Called with profile_mutex. LT_PROFILE_SYNC=on restores synchronous writes
 * for controlled comparisons; ordinary producers never perform storage I/O. */
static void profile_printf_locked(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void profile_printf_locked(const char *format, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, format);
    if (profile_sync) {
        int n = vfprintf(profile_file, format, ap);
        va_end(ap);
        if (n < 0) {
            profile_error = EIO;
            qatomic_set(&it_profile_on, false);
        } else {
            profile_bytes += n;
        }
        return;
    }
    int n = vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(line) || (size_t)n > PROFILE_QUEUE_BYTES - profile_queued) {
        profile_error = ENOBUFS;
        qatomic_set(&it_profile_on, false);
        return;
    }
    size_t tail = (profile_head + profile_queued) % PROFILE_QUEUE_BYTES;
    size_t first = MIN((size_t)n, PROFILE_QUEUE_BYTES - tail);
    memcpy(profile_queue + tail, line, first);
    memcpy(profile_queue, line + first, n - first);
    profile_queued += n;
    profile_bytes += n;
    g_cond_signal(&profile_cond);
}

static gpointer profile_write_thread(gpointer unused)
{
    char buffer[65536];
    int error = 0;
    for (;;) {
        g_mutex_lock(&profile_mutex);
        while (!profile_queued && !profile_closing) {
            g_cond_wait(&profile_cond, &profile_mutex);
        }
        size_t n = MIN(profile_queued, sizeof(buffer));
        size_t first = MIN(n, PROFILE_QUEUE_BYTES - profile_head);
        memcpy(buffer, profile_queue + profile_head, first);
        memcpy(buffer + first, profile_queue, n - first);
        profile_head = (profile_head + n) % PROFILE_QUEUE_BYTES;
        profile_queued -= n;
        bool done = !n && profile_closing;
        g_mutex_unlock(&profile_mutex);
        if (done) {
            break;
        }
        if (!error && fwrite(buffer, 1, n, profile_file) != n) {
            error = EIO;
            g_mutex_lock(&profile_mutex);
            profile_error = error;
            qatomic_set(&it_profile_on, false);
            g_mutex_unlock(&profile_mutex);
        }
    }
    if (fclose(profile_file)) {
        error = EIO;
    }
    if (error) {
        g_mutex_lock(&profile_mutex);
        profile_error = error;
        g_mutex_unlock(&profile_mutex);
    }
    return NULL;
}
static uint64_t profile_started, profile_gl_ns, profile_present_ns;
static struct { uint64_t count, total, max; } profile_slots[0x1010];
static uint32_t profile_touched[0x1010], profile_ntouched;

static void profile_check_locked(void)
{
    if (profile_bytes >= 128 * 1024 * 1024 ||
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - profile_started >= 900000000000ULL) {
        profile_error = profile_error ? profile_error : EFBIG;
        qatomic_set(&it_profile_on, false);
    }
}

static const char *profile_gl_name(uint32_t slot)
{
    switch (slot) {
#define GLES_FN(n, f, id, argc, fl) case id: return #n;
#include "hw/arm/guest-services/gles-names.h"
#undef GLES_FN
    case 0x1000: return "present";
    case 0x1001: return "presentSurface";
    case 0x1002: return "log";
    case 0x1003: return "bindSurface";
    case 0x1004: return "newSharegroup";
    case 0x1005: return "deleteSharegroup";
    case 0x1006: return "newContext";
    case 0x1007: return "deleteContext";
    case 0x1008: return "drawableStorage";
    default: return "unknown";
    }
}

static void profile_flush_gl_locked(uint64_t now)
{
    for (uint32_t i = 0; i < profile_ntouched; i++) {
        uint32_t slot = profile_touched[i];
        profile_printf_locked("G %" PRIu64 " %u %" PRIu64 " %" PRIu64 " %" PRIu64 " %s\n",
                now, slot, profile_slots[slot].count, profile_slots[slot].total,
                profile_slots[slot].max, profile_gl_name(slot));
        memset(&profile_slots[slot], 0, sizeof(profile_slots[slot]));
    }
    profile_ntouched = 0;
}

int qemu_ios_profile_start(const char *path)
{
    int result = 0;
    const char *enabled = getenv("LT_PROFILE");
    g_mutex_lock(&profile_mutex);
    if (!qemu_ios_ui_ready()) {
        result = -EAGAIN;
    } else if (enabled && (!strcmp(enabled, "off") || !strcmp(enabled, "0"))) {
        result = -ENOTSUP;
    } else if (profile_file) {
        result = -EBUSY;
    } else if (!path || !*path) {
        result = -EINVAL;
    } else if (!(profile_file = fopen(path, "wx"))) {
        result = -errno;
    } else {
        setvbuf(profile_file, NULL, _IOFBF, 65536);
        profile_error = 0;
        profile_head = profile_queued = profile_bytes = 0;
        profile_closing = false;
        const char *sync = getenv("LT_PROFILE_SYNC");
        profile_sync = sync && strcmp(sync, "off") && strcmp(sync, "0");
        if (!profile_sync) {
            profile_queue = g_malloc(PROFILE_QUEUE_BYTES);
            profile_thread = g_thread_new("profile-writer", profile_write_thread, NULL);
        }
        profile_gl_ns = profile_present_ns = 0;
        profile_started = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        profile_printf_locked("# LightTouch profile v1; monotonic ns; no GL arguments\n"
                "# F ns serial run outside bql-outside bql-inside vcpu-cpu insns gl present main-cpu due-ns"
                " vcpu-wait vcpu-core vcpu-khz\n"
                "# G end-ns slot count total-ns max-ns name (completed calls since preceding F)\n"
                "# S start-ns end-ns slot context (GL calls >=500us)\n"
                "# W/H start-ns end-ns file line (BQL wait/hold >=500us)\n"
                "# C start-ns end-ns callback (main-loop timer, bottom half or fd handler >=500us)\n"
                "# begin %" PRIu64 "; instruction counters require IT_VCPU_ACCT\n", profile_started);
        it_profile_cb_enable();
        qatomic_set(&it_profile_on, true);
    }
    g_mutex_unlock(&profile_mutex);
    return result;
}

int qemu_ios_profile_stop(void)
{
    int result;
    g_mutex_lock(&profile_mutex);
    if (profile_closing) {
        g_mutex_unlock(&profile_mutex);
        return -EBUSY;
    }
    qatomic_set(&it_profile_on, false);
    if (profile_file) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        profile_flush_gl_locked(now);
        profile_printf_locked("# end %" PRIu64 " error %d\n", now, profile_error);
        profile_closing = true;
        g_cond_signal(&profile_cond);
        GThread *writer = profile_thread;
        if (writer) {
            g_mutex_unlock(&profile_mutex);
            /* Producers are disabled; the writer drains without holding the BQL. */
            g_thread_join(writer);
            g_mutex_lock(&profile_mutex);
        } else if (fclose(profile_file)) {
            profile_error = EIO;
        }
        profile_file = NULL;
        profile_thread = NULL;
        g_clear_pointer(&profile_queue, g_free);
        profile_closing = false;
    }
    result = -profile_error;
    g_mutex_unlock(&profile_mutex);
    return result;
}

int qemu_ios_profile_status(void)
{
    int result;
    g_mutex_lock(&profile_mutex);
    result = profile_error ? -profile_error : profile_file ? 1 : 0;
    g_mutex_unlock(&profile_mutex);
    return result;
}

uint64_t qemu_ios_profile_begin(void)
{
    return qatomic_read(&it_profile_on) ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
}

void qemu_ios_profile_gl(uint64_t start, uint32_t slot, uint32_t context)
{
    uint64_t end = qemu_clock_get_ns(QEMU_CLOCK_REALTIME), dt = end - start;
    g_mutex_lock(&profile_mutex);
    if (qatomic_read(&it_profile_on) && start >= profile_started) {
        profile_gl_ns += dt;
        if (slot == 0x1000 || slot == 0x1001) {
            profile_present_ns += dt;
        }
        if (slot < G_N_ELEMENTS(profile_slots)) {
            if (!profile_slots[slot].count) {
                profile_touched[profile_ntouched++] = slot;
            }
            profile_slots[slot].count++;
            profile_slots[slot].total += dt;
            profile_slots[slot].max = MAX(profile_slots[slot].max, dt);
        }
        if (dt >= 500000) {
            profile_printf_locked("S %" PRIu64 " %" PRIu64 " %u %u\n", start, end, slot, context);
            profile_check_locked();
        }
    }
    g_mutex_unlock(&profile_mutex);
}

void qemu_ios_profile_lock(uint64_t start, uint64_t end, bool wait,
                           const char *file, int line)
{
    if (end - start < 500000 || !qatomic_read(&it_profile_on)) {
        return;
    }
    g_mutex_lock(&profile_mutex);
    if (qatomic_read(&it_profile_on) && start >= profile_started) {
        profile_printf_locked("%c %" PRIu64 " %" PRIu64 " %s %d\n",
                wait ? 'W' : 'H', start, end, file, line);
        profile_check_locked();
    }
    g_mutex_unlock(&profile_mutex);
}

/* Bottom halves by name; timers and fd handlers by symbol, or library offset in a stripped build. */
void qemu_ios_profile_callback(void *fn, const char *name, int64_t ns)
{
    uint64_t end = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    char buf[96];
#ifndef _WIN32
    Dl_info info = { 0 };

    if (!name && dladdr(fn, &info) && info.dli_sname) {
        name = info.dli_sname;
    } else if (!name && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        snprintf(buf, sizeof(buf), "%s+0x%" PRIxPTR, base ? base + 1 : info.dli_fname,
                 (uintptr_t)fn - (uintptr_t)info.dli_fbase);
        name = buf;
    }
#endif
    if (!name) {
        snprintf(buf, sizeof(buf), "%p", fn);
        name = buf;
    }
    g_mutex_lock(&profile_mutex);
    if (qatomic_read(&it_profile_on) && end - ns >= profile_started) {
        profile_printf_locked("C %" PRIu64 " %" PRIu64 " %s\n", end - ns, end, name);
        profile_check_locked();
    }
    g_mutex_unlock(&profile_mutex);
}

static void ios_profile_frame(uint64_t due, uint64_t serial)
{
    int64_t v[6], sched[3];
    struct timespec ts = { 0 };
    if (!qatomic_read(&it_profile_on)) {
        return;
    }
    it_acct_frame_snapshot(v);
    it_acct_vcpu_sched(sched);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    g_mutex_lock(&profile_mutex);
    if (qatomic_read(&it_profile_on)) {
        uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        profile_flush_gl_locked(now);
        profile_printf_locked("F %" PRIu64 " %" PRIu64 " %" PRId64 " %" PRId64
                " %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %" PRIu64
                " %" PRIu64 " %" PRId64 " %" PRIu64 " %" PRId64 " %" PRId64 " %" PRId64 "\n", now, serial,
                v[0], v[1], v[2], MAX((int64_t)0, v[3] - v[2]), v[4], v[5], profile_gl_ns, profile_present_ns,
                (int64_t)ts.tv_sec * (int64_t)1000000000 + (int64_t)ts.tv_nsec, due,
                sched[0], sched[1], sched[2]);
        profile_check_locked();
    }
    g_mutex_unlock(&profile_mutex);
}

static void ios_frame_trace(uint64_t ns, uint64_t serial)
{
    static FILE *trace;
    static bool opened;
    int64_t v[6];
    uint64_t gl_call, gl_present;
    struct timespec ts = { 0 };

    if (!opened) {
        const char *path = getenv("IT_FRAME_TRACE");
        opened = true;
        trace = path && *path ? fopen(path, "a") : NULL;
    }
    if (!trace) {
        return;
    }
    it_acct_frame_snapshot(v);
    gles_host_prof(&gl_call, &gl_present);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    fprintf(trace, "%" PRIu64 " %" PRIu64 " %" PRId64 " %" PRId64 " %" PRId64
            " %" PRId64 " %" PRId64 " %" PRId64 " %" PRIu64 " %" PRIu64 " %" PRId64 "\n",
            ns, serial, v[0], v[1], v[2], v[3], v[4], v[5], gl_call, gl_present,
            (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    fflush(trace);
}

/*
 * Copy the surface rather than handing the app QEMU's pixels directly: the
 * surface can be freed or reallocated by dpy_gfx_switch at any moment, and a
 * UIKit frame in flight referencing freed pixman memory is a use-after-free
 * that would only show up under load. This is the ONLY copy -- the app reads
 * the published buffer in place.
 */
/*
 * A frame's timestamp is the frame interrupt it was latched on, when that is
 * known (ipod_lcd_vsync_due, main loop only): an exact 16.67 ms grid that a
 * front end can present on, where the capture time carries the main loop's
 * dispatch jitter (+/-2 ms on a phone, enough to land a frame on the wrong
 * refresh of a 120 Hz panel). A capture with no fresh vsync (QEMU's display
 * poll, a screendump) is stamped when it happens.
 */
static int64_t ios_vsync_due_ns;

static void ios_note_vsync_due(int64_t realtime_ns)
{
    ios_vsync_due_ns = realtime_ns;
}

static int64_t ios_frame_due(int64_t now)
{
    int64_t due = ios_vsync_due_ns;

    ios_vsync_due_ns = 0;
    return due && due <= now && now - due < 16666667 ? due : now;
}

static void ios_capture(DisplaySurface *surface)
{
    int w = surface_width(surface);
    int h = surface_height(surface);
    int stride = surface_stride(surface);
    const uint8_t *src = surface_data(surface);
    size_t need = (size_t)w * h * 4;
    int next, y;
    uint8_t *dst;

    /*
     * The compare and copy run without the BQL (two passes over 2.4 MiB at 640x960): the surface
     * belongs to the main loop, which is running this, and the buffers to frame_lock.
     * LT_FRAME_COPY_UNLOCKED=off keeps the BQL.
     */
    static int unlocked_copy = -1;
    if (unlocked_copy < 0) {
        const char *v = getenv("LT_FRAME_COPY_UNLOCKED");
        unlocked_copy = !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
    }
    bool unlocked = unlocked_copy && bql_locked();

    ios_init_frame_lock();
    ios_frame_path(IOS_FP_CAPTURE_BEGIN, ios.serial, 0, 0);
    if (unlocked) {
        bql_unlock();
    }
    qemu_mutex_lock(&ios.frame_lock);
    ios_frame_count(&ios_frame_counters.capture_attempts);

    if (need > ios.buf_size) {
        /*
         * Growing (in practice only a rotation) retires the old buffers rather
         * than freeing them: the app may still hold a CGImage over one. They
         * are released at the NEXT growth, seconds away at the very least.
         */
        int i;
        for (i = 0; i < IOS_FRAME_BUFFERS; i++) {
            if (ios.buf[i]) {
                ios.retired = g_slist_prepend(ios.retired, ios.buf[i]);
            }
            ios.buf[i] = g_malloc0(need);
        }
        ios.buf_size = need;
        ios.published = -1;
    }

    /*
     * A panel that redraws whole frames every refresh (s5l8930_display, and
     * any panel while the VM is paused) repaints identical pixels. Publishing
     * them would wake the helper's copy and the app's texture upload for
     * nothing, so an unchanged frame keeps the old serial.
     */
    if (ios.published >= 0 && ios.width == w && ios.height == h) {
        const uint8_t *last = ios.buf[ios.published];
        for (y = 0; y < h; y++) {
            if (memcmp(last + (size_t)y * w * 4, src + (size_t)y * stride,
                       (size_t)w * 4)) {
                break;
            }
        }
        if (y == h) {
            ios_frame_count(&ios_frame_counters.identical_suppressed);
            qemu_mutex_unlock(&ios.frame_lock);
            if (unlocked) {
                bql_lock();
            }
            ios_frame_path(IOS_FP_CAPTURE_SAME, ios.serial, 0, 0);
            return;
        }
    }

    next = (ios.published + 1) % IOS_FRAME_BUFFERS;
    dst = ios.buf[next];
    for (y = 0; y < h; y++) {
        memcpy(dst + (size_t)y * w * 4, src + (size_t)y * stride, (size_t)w * 4);
    }

    ios.width = w;
    ios.height = h;
    ios.published = next;
    ios.serial++;
    ios_frame_count(&ios_frame_counters.publications);
    ios.timestamp_ns = ios_frame_due(qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    /* Stamp before unlocking: a frontend can copy as soon as the lock drops.
     * A later marker can be descheduled and appear after its frontend copy. */
    ios_frame_path(IOS_FP_PUBLISH, ios.serial, ios.timestamp_ns, 0);
    qemu_mutex_unlock(&ios.frame_lock);
    if (unlocked) {
        bql_lock();
    }

    ios_frame_trace(ios.timestamp_ns, ios.serial);
    /* LT_PROFILE_PATH: a headless run records what a phone's frame inspector does, from its first frame. */
    static bool profile_env_done;
    if (!profile_env_done) {
        const char *path = getenv("LT_PROFILE_PATH");
        profile_env_done = true;
        if (path && *path) {
            qemu_ios_profile_start(path);
        }
    }
    ios_profile_frame(ios.timestamp_ns, ios.serial);
    ios_report_first_lit_frame(dst, w, h);

    if (ios.cb) {
        ios.cb(ios.cb_opaque);
    }
}

/*
 * Note what changed; do not publish yet.
 *
 * Now that the panel does proper dirty tracking, one guest frame arrives as
 * SEVERAL partial updates. Publishing each one hands the app a frame caught
 * mid-paint, which is visible as the screen wiping top-to-bottom instead of
 * changing at once -- and costs a full-surface copy per fragment rather than
 * per frame. Portable hosts defer publication until the panel paint returns;
 * Apple hosts retain the original display-refresh publication boundary.
 */
static void ios_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    ios.pending = true;
#ifndef __APPLE__
    if (ios.publish_timer && !timer_pending(ios.publish_timer)) {
        timer_mod(ios.publish_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    }
#endif
}

static void ios_gfx_switch(DisplayChangeListener *dcl, DisplaySurface *surface)
{
    /*
     * A new surface replaces everything, so it is whole by definition -- but
     * do NOT capture it here: on a rotation the console resize happens in the
     * middle of the panel's refresh, BEFORE it has blitted anything into the
     * new surface, so capturing now publishes one all-black frame at the new
     * size and the app visibly blinks. Mark it pending without scheduling a
     * capture here: the ensuing dirty update schedules portable publication
     * after the paint returns; ios_refresh remains the fallback boundary.
     */
    if (surface) {
        ios.pending = true;
    }
}

#ifndef __APPLE__
/* Adapted from upstream 97860b36b0e9fcdd008e95e58efff9609ca44aee.
 * Coalesce dirty rectangles until the whole panel paint has returned, rather
 * than resampling its vsync at QEMU's 30 ms display-refresh interval. */
static void ios_publish(QemuConsole *con)
{
    DisplaySurface *surface;

    if (!ios.attached || !qatomic_read(&ios_vm_alive) || !con ||
        !ios.pending) {
        return;
    }
    surface = qemu_console_surface(con);
    if (surface) {
        ios_capture(surface);
        ios.pending = false;
    }
}

/*
 * A main-loop timer, not a bottom half: ios_capture drops the BQL, and a bottom half runs inside
 * qemu_aio_context dispatch, where a vCPU taking the BQL could poll the same context and resume
 * a block coroutine twice.
 */
static void ios_publish_timer(void *opaque)
{
    ios_publish(ios.con);
}

static void ios_cancel_publish(bool stopping)
{
    if (ios.publish_timer) {
        timer_del(ios.publish_timer);
        if (stopping) {
            timer_free(ios.publish_timer);
            ios.publish_timer = NULL;
        }
    }
    ios.pending = false;
}
#endif

static void ios_refresh(DisplayChangeListener *dcl)
{
#ifndef __APPLE__
    /* Display cleanup can still notify listeners after the main loop ends. */
    if (!ios.attached || !qatomic_read(&ios_vm_alive)) {
        return;
    }
    graphic_hw_update(dcl->con);
    ios_publish(dcl->con);
#else
    DisplaySurface *surface;

    graphic_hw_update(dcl->con);

    if (!ios.pending) {
        return;
    }
    surface = qemu_console_surface(dcl->con);
    if (surface) {
        ios_capture(surface);
        ios.pending = false;
    }
#endif
}

static const DisplayChangeListenerOps ios_dcl_ops = {
    .dpy_name       = "ios",
    .dpy_refresh    = ios_refresh,
    .dpy_gfx_update = ios_gfx_update,
    .dpy_gfx_switch = ios_gfx_switch,
};

/*
 * Called from qemu_ios_main() once qemu_init() has returned, on the QEMU
 * thread with the BQL held.
 *
 * This cannot be driven from the app side. Before qemu_init() there is no AIO
 * context and no console, so scheduling the registration as a bottom half from
 * the app -- the obvious design -- dereferences a null context and segfaults
 * on the app's main thread.
 */
/*
 * Whether the emulator is up and its AioContext is safe to schedule onto.
 *
 * qemu_get_aio_context() returns NULL until qemu_init() has run, and after the
 * main loop returns the context is no longer serviced -- so a bottom half
 * scheduled outside that window either dereferences NULL on the app's main
 * thread or is silently dropped (leaking its payload). The app starts QEMU on a
 * background thread and shows its window immediately, so key/mouse/tilt events
 * genuinely can arrive on both sides of that window. Every ABI entry point that
 * schedules work checks this first.
 */
bool qemu_ios_ui_ready(void)
{
    return qatomic_read(&ios_vm_alive) != 0;
}

bool qemu_ios_ui_guest_shutdown_confirmed(void)
{
    return qatomic_read(&ios_is_ipad1) ? s5l8930_d1815_guest_shutdown_confirmed()
                                       : pcf50633_guest_shutdown_confirmed();
}

bool qemu_ios_ui_display_sleeping(void)
{
    /* The iPad has no iPod LCD backlight; its panel is powered over DSI. */
    return qatomic_read(&ios_dsi_panel) ? ipod_touch_mipi_dsi_panel_off()
                                       : lcd_backlight_is_off();
}

int qemu_ios_ui_backlight_level(void)
{
    return ios_backlight_level();
}

bool qemu_ios_ui_storage_failed(void)
{
    return ipod_touch_fmss_io_failed() || ipod_touch_nor_io_failed() ||
           s5l8930_iop_io_failed();
}

void qemu_ios_storage_stats(QemuIosStorageStats *out)
{
    NANDStorageStats s;

    nand_storage_stats(&s);
    *out = (QemuIosStorageStats) {
        .read_ops = s.ops[NAND_STAT_READ], .read_bytes = s.bytes[NAND_STAT_READ], .read_ns = s.ns[NAND_STAT_READ],
        .write_ops = s.ops[NAND_STAT_WRITE], .write_bytes = s.bytes[NAND_STAT_WRITE],
        .write_ns = s.ns[NAND_STAT_WRITE],
        .erase_ops = s.ops[NAND_STAT_ERASE], .erase_ns = s.ns[NAND_STAT_ERASE],
    };
}

void qemu_ios_ui_vm_stopped(void)
{
#if !defined(__APPLE__)
    qatomic_set(&ios_vm_alive, 0);
    /* Called under BQL before qemu_cleanup: a deleted BH cannot capture a
     * surface or notify the app during console/AioContext teardown. */
    ios_cancel_publish(true);
    ios_input_discard_pending();
    qemu_ios_ui_cancel_input();
#else
    qemu_ios_ui_cancel_input();
    qatomic_set(&ios_vm_alive, 0);
#endif
    /* Also drop `attached`. It used to stay true for the life of the process,
     * so every entry point below that guarded on it kept accepting work after
     * the main loop had gone -- most damagingly snapshot_save2, which then
     * queued a BH nobody would ever run and pinned its status at RUNNING,
     * stalling the app's save poll for its full timeout on every quit that
     * races the VM exiting. */
    ios.attached = false;
}

void qemu_ios_ui_vm_started(void)
{
    int i;

    if (ios.attached) {
        return;
    }
    ios.con = qemu_console_lookup_by_index(0);
    qatomic_set(&ios_is_ipad1, object_dynamic_cast(qdev_get_machine(),
                                                   MACHINE_TYPE_NAME("ipad1")) != NULL);
    qatomic_set(&ios_dsi_panel, qatomic_read(&ios_is_ipad1) ||
                object_dynamic_cast(qdev_get_machine(), "s5l8920-machine") != NULL);
    for (i = 0; i < INPUT_EVENT_SLOTS_MAX; i++) {
        ios.slots[i].tracking_id = -1;
    }
    if (ios.con) {
        ios.dcl.ops = &ios_dcl_ops;
        ios.dcl.con = ios.con;
#ifndef __APPLE__
        ios.publish_timer = timer_new_ns(QEMU_CLOCK_REALTIME, ios_publish_timer, NULL);
#endif
        register_displaychangelistener(&ios.dcl);
        ipod_lcd_vsync_due = ios_note_vsync_due;
    }
    qatomic_set(&portable_paused, !runstate_is_running());
    qemu_add_vm_change_state_handler(portable_runstate_changed, NULL);
    ios.attached = true;
    ios_add_frame_counter_property();
    qemu_register_reset(ios_sequence_reset, NULL);
    portable_has_gyro = object_dynamic_cast(qdev_get_machine(), MACHINE_TYPE_NAME("iPod-Touch-4G")) ||
                        object_dynamic_cast(qdev_get_machine(), MACHINE_TYPE_NAME("iPhone-4"));
    qatomic_set(&ios_vm_alive, 1);
}

/* --- app thread -------------------------------------------------------- */

/*
 * Every machine's facts for the app (qemu-ios-ui.h). Panel limits are the
 * machines' panel= setters': the CLCD boards' even width 64..1024 by 64..511
 * rows (the S5L8720 window keeps 9 bits of height), the A4 pipe's width a
 * multiple of 16 with both sides 64..2047, inside iBoot's display region
 * (9 MB at 4 bytes a pixel, 0x4f700000 up to the iPad's DRAM end).
 */
#define CLCD_PANEL 64, 1024, 511, 2, 0
#define A4_PANEL   64, 2047, 2047, 16, 0x900000 / 4
static const QemuIosDeviceInfo ios_devices[] = {
    /* machine          board    w     h    scale orient cell   usbhost compass charger panel */
    { "iPod-Touch",    "n72ap",  320,  480, 1, 0, false, false, false, false, CLCD_PANEL },
    { "iPod-Touch-1G", "n45ap",  320,  480, 1, 0, false, false, false, false, CLCD_PANEL },
    { "iPhone-2G",     "m68ap",  320,  480, 1, 0, true,  false, false, false, CLCD_PANEL },
    { "n18",           "n18ap",  320,  480, 1, 0, false, false, false, false, CLCD_PANEL },
    { "n88",           "n88ap",  320,  480, 1, 0, true,  false, true,  false, CLCD_PANEL },
    /* s5l8930_display scans out 1024x768: the K48 panel is mounted landscape */
    { "ipad1",         "k48ap", 1024,  768, 1, 1, false, true,  true,  true,  A4_PANEL },
    { "iPod-Touch-4G", "n81ap",  640,  960, 2, 0, false, true,  false, false, A4_PANEL },
    { "iPhone-4",      "n90ap",  640,  960, 2, 0, true,  true,  true,  false, A4_PANEL },
};

const QemuIosDeviceInfo *qemu_ios_device_info(const char *machine)
{
    for (size_t i = 0; machine && i < ARRAY_SIZE(ios_devices); i++) {
        if (!strcmp(ios_devices[i].machine, machine)) {
            return &ios_devices[i];
        }
    }
    return NULL;
}

const QemuIosDeviceInfo *qemu_ios_device_info_at(int index)
{
    return index >= 0 && index < (int)ARRAY_SIZE(ios_devices) ? &ios_devices[index] : NULL;
}

void qemu_ios_ui_attach(qemu_ios_frame_cb cb, void *opaque)
{
    ios_init_frame_lock();
    ios.cb = cb;
    ios.cb_opaque = opaque;
}

/*
 * Hand back a pointer to the newest frame, without copying it.
 *
 * `serial` is in-out: pass what you last saw and this returns false, touching
 * nothing, if that is still the newest. That check matters more than it looks
 * -- the guest paints at 30-60 Hz while an iPhone display link fires at up to
 * 120, so most wake-ups have no new frame at all and used to spend a 600 KB
 * allocation and copy discovering it.
 *
 * The pointer stays readable for the next few frames (see IOS_FRAME_BUFFERS).
 * Do not free it and do not hold it indefinitely.
 */
bool qemu_ios_ui_frame(const void **pixels, int *width, int *height,
                       uint64_t *serial)
{
    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);

    if (ios.published < 0 || ios.serial == *serial) {
        qemu_mutex_unlock(&ios.frame_lock);
        return false;
    }

    *pixels = ios.buf[ios.published];
    *width = ios.width;
    *height = ios.height;
    *serial = ios.serial;

    qemu_mutex_unlock(&ios.frame_lock);
    return true;
}

void qemu_ios_ui_frame_size(int *width, int *height)
{
    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);
    *width = ios.width;
    *height = ios.height;
    qemu_mutex_unlock(&ios.frame_lock);
}

struct ios_touch {
    double nx, ny;             /* normalized 0..1 over the panel */
    bool down;
};

/*
 * Deliberately the LEGACY absolute-pointer path, not QEMU's multi-touch
 * events, even though the panel model understands both.
 *
 * console_handle_touch_event() -- the obvious API -- queues the BEGIN commit
 * BEFORE the DATA events carrying the coordinates, and the digitizer ignores a
 * commit for a slot whose position it has never seen. The press is therefore
 * dropped and only the release survives, which reads exactly like "taps do
 * nothing". The abs+button sequence below is the one the existing tooling
 * (qmp-touch.py) has always used.
 *
 * The cost is one finger at a time: pinch and rotate need the multi-touch path
 * with DATA queued before the commit.
 */
static void ios_touch_bh(void *opaque)
{
    struct ios_touch *t = opaque;

    ios_sequence_cancel_current();
    if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED)) {
        /* Release an already delivered contact without advancing paused input. */
        if (!t->down && ios_manual_touch && ios.con) {
            InputBtnEvent button = {.button = INPUT_BUTTON_LEFT, .down = false};
            InputEvent event = {.type = INPUT_EVENT_KIND_BTN,
                                .u.btn.data = &button};
            qemu_input_event_send_impl(ios.con, &event);
            qemu_input_event_sync_impl();
        }
        if (!t->down) {
            ios_manual_touch = false;
        }
        g_free(t);
        return;
    }
    ios_manual_touch = t->down;
    if (ios.con) {
        qemu_input_queue_abs(ios.con, INPUT_AXIS_X,
                             (int)(t->nx * INPUT_EVENT_ABS_MAX), 0,
                             INPUT_EVENT_ABS_MAX);
        qemu_input_queue_abs(ios.con, INPUT_AXIS_Y,
                             (int)(t->ny * INPUT_EVENT_ABS_MAX), 0,
                             INPUT_EVENT_ABS_MAX);
        qemu_input_queue_btn(ios.con, INPUT_BUTTON_LEFT, t->down);
        qemu_input_event_sync();
    }
    g_free(t);
}

void qemu_ios_ui_touch(int slot, int phase, double nx, double ny)
{
    struct ios_touch *t;

    if (slot != 0 || phase < QEMU_IOS_TOUCH_BEGIN || phase > QEMU_IOS_TOUCH_END ||
        !isfinite(nx) || !isfinite(ny) || nx < 0 || nx > 1 || ny < 0 || ny > 1 ||
        !qemu_ios_ui_ready()) {
        return;                 /* one finger; no AIO context before attach */
    }

    t = g_new0(struct ios_touch, 1);
    t->nx = nx;
    t->ny = ny;
    t->down = (phase != QEMU_IOS_TOUCH_END);
    if (!ios_input_enqueue(ios_touch_bh, t, g_free)) {
        g_free(t);
    }
}

/* Upstream's name for the ordered host input queue (qemu-ios-ui.h); the Mac extras use it. */
void qemu_ios_ui_input_later(void (*fn)(void *), void *opaque)
{
    if (!ios_input_enqueue(fn, opaque, g_free)) {
        g_free(opaque);
    }
}

/* --- hardware buttons -------------------------------------------------- */

struct ios_button {
    int button;
    bool down;
};

static struct ios_button_hold {
    QEMUTimer *release;
    int64_t pressed_at;
    IPodTouchButton button;
    bool down;
} ios_button_holds[4];

static void ios_press(IPodTouchButton button, bool down)
{
    /* Each is a no-op unless its machine is the one running. */
    ipod_touch_press_button(button, down);
    ipad1_press_button(button, down);
    ipod_touch_1g_press_button(button, down);
    s5l8920_press_button(button, down);
}

static void ios_button_release(void *opaque)
{
    struct ios_button_hold *hold = opaque;
    ios_press(hold->button, false);
    hold->down = false;
}

static void ios_button_bh(void *opaque)
{
    struct ios_button *b = opaque;
    IPodTouchButton button;
    switch (b->button) {
    case QEMU_IOS_BUTTON_HOME:
        button = IPOD_TOUCH_BUTTON_HOME;
        break;
    case QEMU_IOS_BUTTON_POWER:
        button = IPOD_TOUCH_BUTTON_POWER;
        break;
    case QEMU_IOS_BUTTON_VOLUME_UP:
        button = IPOD_TOUCH_BUTTON_VOLUP;
        break;
    case QEMU_IOS_BUTTON_VOLUME_DOWN:
        button = IPOD_TOUCH_BUTTON_VOLDOWN;
        break;
    default:
        g_free(b);
        return;
    }
    ios_sequence_cancel_current();
    struct ios_button_hold *hold = &ios_button_holds[b->button];
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (!hold->release) {
        hold->button = button;
        hold->release = timer_new_ms(QEMU_CLOCK_VIRTUAL, ios_button_release, hold);
    }
    if (b->down) {
        timer_del(hold->release);
        hold->pressed_at = now;
        hold->down = true;
        ios_press(button, true);
    } else {
        /* Both host events can arrive in one BH batch under load. Give the
         * guest's debounce handler time to observe the pressed pin. */
        timer_mod(hold->release, MAX(now, hold->pressed_at + 100));
    }
    g_free(b);
}

void qemu_ios_ui_button(int button, bool down)
{
    struct ios_button *b;

    if (button < QEMU_IOS_BUTTON_HOME || button > QEMU_IOS_BUTTON_VOLUME_DOWN ||
        !qemu_ios_ui_ready()) {
        return;
    }
    b = g_new0(struct ios_button, 1);
    b->button = button;
    b->down = down;
    if (!ios_input_enqueue(ios_button_bh, b, g_free)) {
        g_free(b);
    }
}

/* --- generic host input sequence ---------------------------------------- */
/* All event application and ownership are under the BQL. Publication is atomic;
 * the host only observes the most recently submitted sequence ID/status. */
static IosInputSequence ios_sequence;
static QEMUTimer *ios_sequence_timer;
static uint64_t ios_sequence_id;
static int ios_sequence_status;
static QemuSeqLock ios_sequence_publication;

/* Single BQL writer; host readers observe one coherent ID/status pair. */
static void ios_sequence_publish(uint64_t id, int status)
{
    seqlock_write_begin(&ios_sequence_publication);
    qatomic_set(&ios_sequence_id, id);
    qatomic_set(&ios_sequence_status, status);
    seqlock_write_end(&ios_sequence_publication);
}

static void ios_sequence_emit(void *opaque, const IosInputEvent *e)
{
    if (e->kind == 0) {
        ios_press((IPodTouchButton)e->value, !!e->phase);

    } else if (ios.con) {
        qemu_input_queue_abs(ios.con, INPUT_AXIS_X,
            (int)(e->x * INPUT_EVENT_ABS_MAX), 0, INPUT_EVENT_ABS_MAX);
        qemu_input_queue_abs(ios.con, INPUT_AXIS_Y,
            (int)(e->y * INPUT_EVENT_ABS_MAX), 0, INPUT_EVENT_ABS_MAX);
        qemu_input_queue_btn(ios.con, INPUT_BUTTON_LEFT, e->phase != 2);
        qemu_input_event_sync();
    }
}

/* qemu_input_event_send() deliberately drops ordinary events while paused.
 * Cancellation is ownership cleanup, so its release must reach the handler
 * under BQL even when virtual time is stopped; it must not leave a finger
 * held forever after resume. No coordinate/button-down is synthesized here. */
static void ios_sequence_release_emit(void *opaque, const IosInputEvent *e)
{
    if (e->kind == 1 && ios.con) {
        InputBtnEvent button = {.button = INPUT_BUTTON_LEFT, .down = false};
        InputEvent event = {.type = INPUT_EVENT_KIND_BTN, .u.btn.data = &button};
        qemu_input_event_send_impl(ios.con, &event);
        qemu_input_event_sync_impl();
    } else {
        ios_sequence_emit(opaque, e);
    }
}

static void ios_sequence_cancel_current(void)
{
    if (ios_sequence_timer) {
        timer_del(ios_sequence_timer);
    }
    ios_input_cancel(&ios_sequence, ios_sequence_release_emit, NULL);
    if (qatomic_read(&ios_sequence_status) == QEMU_IOS_INPUT_RUNNING) {
        ios_sequence_publish(qatomic_read(&ios_sequence_id), QEMU_IOS_INPUT_CANCELLED);
    }
}

void qemu_ios_ui_manual_touch2(bool down)
{
    ios_sequence_cancel_current();
    ios_manual_touch2 = down;
}

void qemu_ios_ui_cancel_input(void)
{
    ios_sequence_cancel_current();
    portable_release_all();
}

static void ios_sequence_tick(void *opaque)
{
    if (ios_input_step(&ios_sequence, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                       ios_sequence_emit, NULL)) {
        ios_sequence_publish(qatomic_read(&ios_sequence_id), QEMU_IOS_INPUT_DONE);
    } else {
        timer_mod(ios_sequence_timer, ios_sequence.origin +
                  ios_sequence.events[ios_sequence.next].at_ms);
    }
}

typedef struct {
    uint64_t id;
    size_t count;
    IosInputEvent events[IOS_INPUT_MAX_EVENTS];
} IosSequenceRequest;

static void ios_sequence_submit_bh(void *opaque)
{
    IosSequenceRequest *r = opaque;
    ios_sequence_cancel_current();
    ios_sequence_publish(0, QEMU_IOS_INPUT_REJECTED);
    bool manual = ios_manual_touch || ios_manual_touch2;
    for (int i = 0; i < 4; ++i) {
        manual |= ios_button_holds[i].down;
    }
    if (!manual && ios.con && qemu_ios_ui_ready()) {
        memcpy(ios_sequence.events, r->events, r->count * sizeof(r->events[0]));
        ios_sequence.count = r->count;
        ios_sequence.next = 0;
        ios_sequence.origin = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
        if (!ios_sequence_timer) {
            ios_sequence_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                               ios_sequence_tick, NULL);
        }
        ios_sequence_publish(0, QEMU_IOS_INPUT_RUNNING);
        /* Even offset zero is scheduled on guest time: a paused guest does
         * not acquire a button until its virtual clock resumes. */
        timer_mod(ios_sequence_timer, ios_sequence.origin + r->events[0].at_ms);
    }
    ios_sequence_publish(r->id, qatomic_read(&ios_sequence_status));
    g_free(r);
}

bool qemu_ios_ui_input_sequence(uint64_t id, size_t count,
    const int64_t *at_ms, const int32_t *kind, const int32_t *value,
    const int32_t *phase, const double *x, const double *y)
{
    if (!id || !qemu_ios_ui_ready() || !count || count > IOS_INPUT_MAX_EVENTS ||
        !at_ms || !kind || !value || !phase || !x || !y) {
        return false;
    }
    IosSequenceRequest *r = g_new0(IosSequenceRequest, 1);
    r->id = id;
    r->count = count;
    for (size_t i = 0; i < count; ++i) {
        if (kind[i] != 0 && kind[i] != 1) { g_free(r); return false; }
        r->events[i] = (IosInputEvent){at_ms[i], kind[i], value[i], phase[i],
                                     x[i], y[i]};
    }
    if (!ios_input_valid(r->events, count)) {
        g_free(r);
        return false;
    }
    if (!ios_input_enqueue(ios_sequence_submit_bh, r, g_free)) {
        g_free(r);
    }
    return true;
}

int qemu_ios_ui_input_sequence_status(uint64_t id)
{
    unsigned sequence;
    uint64_t published;
    int status;
    do {
        sequence = seqlock_read_begin(&ios_sequence_publication);
        published = qatomic_read(&ios_sequence_id);
        status = qatomic_read(&ios_sequence_status);
    } while (seqlock_read_retry(&ios_sequence_publication, sequence));
    return id && published == id ? status : QEMU_IOS_INPUT_UNKNOWN;
}

static void ios_sequence_cancel_bh(void *opaque)
{
    uint64_t *id = opaque;
    if (*id == qatomic_read(&ios_sequence_id)) {
        ios_sequence_cancel_current();
    }
    g_free(id);
}

void qemu_ios_ui_input_sequence_cancel(uint64_t id)
{
    if (id && qemu_ios_ui_ready()) {
        uint64_t *copy = g_new(uint64_t, 1);
        *copy = id;
        if (!ios_input_enqueue(ios_sequence_cancel_bh, copy, g_free)) {
            g_free(copy);
        }
    }
}

/* --- snapshots ---------------------------------------------------------- */

/*
 * Save the whole machine to a file, so the next launch can resume instead of
 * spending half a minute booting.
 *
 * This machine has no block device, so the classic savevm/loadvm path is
 * unavailable -- the migration stream to a `file:` URI is the equivalent, and
 * restoring is just `-incoming file:...` on the next run (plus the autostart
 * fix in qemu-ios-entry.c, or the machine comes back paused).
 *
 * The guest is stopped first: a snapshot taken while the vCPU is running would
 * capture RAM that disagrees with itself.
 *
 * The status distinguishes "not started", "in flight", "done" and "failed": it
 * is set RUNNING on the app thread before the BH so the pre-BH window reads
 * RUNNING, then resolved by reading the migration state.
 */
static int ios_snap_status = QEMU_IOS_SNAPSHOT_IDLE;
static char ios_snap_err[256];
/*
 * Whether THIS save's bottom half has run and its qmp_migrate was accepted.
 *
 * Without it, status() resolved RUNNING by reading migrate_get_current()->state
 * -- a global only reset to SETUP inside qmp_migrate, i.e. inside the BH. After
 * one successful save it stays COMPLETED forever, so the SECOND save in a
 * process read COMPLETED before its BH had run and reported DONE instantly. The
 * app then ran its atomic promote: deleted the good snapshot and renamed a .tmp
 * that did not exist yet -- silent total loss of the saved state, reported as
 * success. Gate the migration-state read on the BH having actually started.
 */
static int ios_snap_bh_ran;

static void ios_snapshot2_bh(void *opaque)
{
    ios_sequence_cancel_current();
    char *path = opaque;
    Error *err = NULL;
    g_autofree char *uri = g_strdup_printf("file:%s", path);

    if (qemu_ios_ui_storage_failed()) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "NAND storage write failed");
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
        g_free(path);
        return;
    }

    qmp_stop(&err);
    if (err) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "stop: %s", error_get_pretty(err));
        error_free(err);
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
        g_free(path);
        return;
    }
    /*
     * Do NOT migrate the global-state section. It would record the runstate we
     * just stopped into ("paused"), and on restore process_incoming_migration_bh
     * sees a non-live target runstate and parks the machine paused forever --
     * autostart (qemu-ios-entry.c) is bypassed and the guest comes back frozen:
     * no vCPU, no input, no orientation. With the section omitted the
     * destination assumes RUNNING (migration_get_target_runstate) and autostart
     * resumes the vCPU the instant the incoming stream is consumed. We never use
     * suspend, so nothing else in global-state is needed.
     */
    migrate_get_current()->store_global_state = false;
    qmp_migrate(uri, false, NULL, false, false, &err);
    if (err) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "save: %s", error_get_pretty(err));
        error_free(err);
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
    } else {
        /* Migration accepted: only NOW may status() trust the migration state. */
        qatomic_set(&ios_snap_bh_ran, 1);
        fprintf(stderr, "[snapshot] writing %s\n", path);
    }
    g_free(path);
}

void qemu_ios_snapshot_save2(const char *path)
{
    if (!qemu_ios_ui_ready()) {
        snprintf(ios_snap_err, sizeof(ios_snap_err), "device not attached");
        qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_FAILED);
        return;
    }
    ios_snap_err[0] = '\0';
    qatomic_set(&ios_snap_bh_ran, 0);
    /* Set RUNNING here, on the app thread, so a status() before the BH runs
     * does not read a leftover DONE/IDLE. */
    qatomic_set(&ios_snap_status, QEMU_IOS_SNAPSHOT_RUNNING);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_snapshot2_bh, g_strdup(path));
}

QemuIosSnapshotStatus qemu_ios_snapshot_status(char *errbuf, unsigned long errlen)
{
    int s = qatomic_read(&ios_snap_status);
    if (s == QEMU_IOS_SNAPSHOT_RUNNING && qatomic_read(&ios_snap_bh_ran)) {
        /* Resolve against the migration state -- but only once this save's BH
         * has actually started a migration, or we would read the PREVIOUS
         * save's leftover COMPLETED. Reading the enum field is a plain int
         * load; good enough for a status poll. */
        MigrationState *ms = migrate_get_current();
        int st = ms ? (int)ms->state : 0;
        if (st == MIGRATION_STATUS_COMPLETED) {
            s = QEMU_IOS_SNAPSHOT_DONE;
            qatomic_set(&ios_snap_status, s);
        } else if (st == MIGRATION_STATUS_FAILED || st == MIGRATION_STATUS_CANCELLED) {
            snprintf(ios_snap_err, sizeof(ios_snap_err), "migration did not complete (state %d)", st);
            s = QEMU_IOS_SNAPSHOT_FAILED;
            qatomic_set(&ios_snap_status, s);
        }
    }
    if (errbuf && errlen) {
        strncpy(errbuf, ios_snap_err, errlen - 1);
        errbuf[errlen - 1] = '\0';
    }
    return s;
}

/*
 * Restart the vCPU after a save. A completed save (a migration to a file)
 * inactivates every block node so a destination could take them over; the
 * A4/S5L8920 NAND and the NOR are block backends, so the guest's next write
 * after a bare vm_start() hit bdrv_co_write_req_prepare's BDRV_O_INACTIVE
 * assertion and took the process down. Take them back first, as qmp_cont does.
 */
static void ios_restart_after_save(void)
{
    Error *err = NULL;

    if (qemu_ios_ui_storage_failed() || runstate_is_running()) {
        return;
    }
    if (!migration_block_activate(&err)) {
        fprintf(stderr, "[snapshot] resume: %s\n", error_get_pretty(err));
        error_free(err);
        return;
    }
    vm_start();
}

static void ios_snapshot_resume_bh(void *opaque)
{
    ios_restart_after_save();
}

void qemu_ios_snapshot_resume(void)
{
    if (!qemu_ios_ui_ready()) {
        return;   /* NULL AioContext before qemu_init(); the only entry point
                   * here that was missing this check. */
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_snapshot_resume_bh, NULL);
}

/* --- foreground/background ---------------------------------------------- */

/*
 * iOS terminates an app that touches GL while it is not foreground, and the
 * emulated CPU has no notion of app lifecycle -- it keeps executing guest code,
 * including whatever GL the guest is in the middle of. The app tells us when
 * that is no longer safe.
 *
 * Deliberately NOT scheduled through a bottom half: by the time a BH runs, the
 * app may already have been backgrounded and the first illegal GL call made.
 * The flag is a plain bool read on the vCPU thread; a stale read costs one
 * rejected call, whereas being late costs the process.
 */
static void ios_resume_bh(void *opaque)
{
    /*
     * Saving a snapshot stops the machine and leaves it stopped -- migration
     * refuses to run from a live VM, and refuses a SECOND save from one it
     * already paused ("Can't migrate the vm that was paused due to previous
     * migration"). So coming back to the foreground has to restart it, or the
     * guest is frozen from the first time the app was backgrounded onwards.
     */
    ios_restart_after_save();
}

void qemu_ios_set_foreground(bool foreground)
{
    gles_host_set_allowed(foreground);

    if (foreground && qemu_ios_ui_ready()) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), ios_resume_bh, NULL);
    }
}

/* Portable frame/board ABI. All output storage is caller owned. */
uint32_t qemu_ios_portable_version(void) { return QEMU_IOS_PORTABLE_VERSION; }
bool qemu_ios_ui_paused(void) { return qemu_ios_ui_ready() && qatomic_read(&portable_paused); }
int qemu_ios_portable_board(const char *machine, QemuIosPortableBoard *out)
{
    const QemuIosDeviceInfo *d = qemu_ios_device_info(machine);
    if (!out || out->size < sizeof(*out) || out->version != 1 || !d) return QEMU_IOS_INVALID;
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out); out->version = 1;
    pstrcpy(out->machine, sizeof(out->machine), d->machine);
    pstrcpy(out->board, sizeof(out->board), d->board);
    out->width = d->screen_width; out->height = d->screen_height;
    out->scale = d->screen_scale; out->orientation = d->default_orientation;
    out->max_contacts = 5;
    out->capabilities = QEMU_IOS_CAP_ACCEL | QEMU_IOS_CAP_MULTITOUCH | QEMU_IOS_CAP_FRAME_COPY;
    if (!strcmp(d->board, "n81ap") || !strcmp(d->board, "n90ap")) out->capabilities |= QEMU_IOS_CAP_GYRO;
    /* boards whose codec model reports a headphone jack: machine option headphones=on */
    if (!strcmp(d->board, "n81ap")) out->capabilities |= QEMU_IOS_CAP_HEADPHONES;
    return QEMU_IOS_OK;
}
int qemu_ios_portable_board_at(int index, QemuIosPortableBoard *out)
{
    const QemuIosDeviceInfo *d = qemu_ios_device_info_at(index);
    return d ? qemu_ios_portable_board(d->machine, out) : QEMU_IOS_UNSUPPORTED;
}
int qemu_ios_portable_frame_copy(void *pixels, size_t capacity,
                               uint64_t last_serial, QemuIosPortableFrame *out)
{
    if (!out || out->size < sizeof(*out) || out->version != 1) return QEMU_IOS_INVALID;
    ios_init_frame_lock();
    qemu_mutex_lock(&ios.frame_lock);
    ios_frame_count(&ios_frame_counters.valid_copy_calls);
    if (ios.published < 0 || ios.serial == last_serial) {
        ios_frame_count(&ios_frame_counters.no_frame);
        qemu_mutex_unlock(&ios.frame_lock); return QEMU_IOS_NO_FRAME;
    }
    *out = (QemuIosPortableFrame){ .size = sizeof(*out), .version = 1,
        .width = ios.width, .height = ios.height, .stride = ios.width * 4,
        .format = 1, .serial = ios.serial, .timestamp_ns = ios.timestamp_ns,
        .required_bytes = (uint64_t)ios.width * ios.height * 4 };
    int result = QEMU_IOS_BUFFER_SMALL;
    if (pixels && capacity >= out->required_bytes) {
        memcpy(pixels, ios.buf[ios.published], out->required_bytes);
        ios_frame_count(&ios_frame_counters.pixel_copies);
        ios_frame_counters.last_copied_serial = ios.serial;
        result = QEMU_IOS_OK;
    } else if (!pixels) {
        ios_frame_count(&ios_frame_counters.metadata_only);
    } else {
        ios_frame_count(&ios_frame_counters.buffer_small);
    }
    qemu_mutex_unlock(&ios.frame_lock);
    return result;
}

/* Stable host IDs map to controller slots, never to transient pointer indices. */
static struct { bool active; uint32_t id; double x, y; } portable_contacts[5];
static uint64_t portable_touch_timestamp, portable_motion_timestamp[2];
static uint64_t portable_status_id;
static int portable_status, portable_pending;
/*
 * Board devices outlive every input event, so each is looked up once (after
 * ui-ready, when the machine exists). object_resolve_path_type walks the whole
 * QOM tree: per 100 Hz motion sample it was a quarter of the main loop's time,
 * all under the BQL.
 */
static Object *portable_device(Object **cache, bool *resolved, const char *type)
{
    if (!*resolved) {
        *cache = object_resolve_path_type("", type, NULL);
        *resolved = true;
    }
    return *cache;
}
static LIS302DLState *portable_accel(void)
{
    static Object *obj;
    static bool resolved;
    portable_device(&obj, &resolved, TYPE_LIS302DL);
    return obj ? LIS302DL(obj) : NULL;
}
static void portable_emit(int slot, int phase, double x, double y)
{
    /* Send directly under BQL so releases also work while paused. Coordinates
     * always precede commit. Controller models apply their existing transforms. */
    InputMultiTouchEvent mtt = { .slot = slot, .tracking_id = slot + 1 };
    InputEvent event = { .type = INPUT_EVENT_KIND_MTT, .u.mtt.data = &mtt };
    if (phase != QEMU_IOS_TOUCH_END && phase != QEMU_IOS_TOUCH_CANCEL) {
        mtt.type = INPUT_MULTI_TOUCH_TYPE_DATA; mtt.axis = INPUT_AXIS_X;
        mtt.value = x * INPUT_EVENT_ABS_MAX;
        qemu_input_event_send_impl(ios.con, &event);
        mtt.axis = INPUT_AXIS_Y; mtt.value = y * INPUT_EVENT_ABS_MAX;
        qemu_input_event_send_impl(ios.con, &event);
    }
    mtt.type = phase == QEMU_IOS_TOUCH_BEGIN ? INPUT_MULTI_TOUCH_TYPE_BEGIN :
               phase == QEMU_IOS_TOUCH_UPDATE ? INPUT_MULTI_TOUCH_TYPE_UPDATE : INPUT_MULTI_TOUCH_TYPE_END;
    qemu_input_event_send_impl(ios.con, &event);
}
static void portable_release_input(bool preserve_buttons)
{
    if (ios.con) {
        for (int i = 0; i < 5; i++) {
            if (portable_contacts[i].active) portable_emit(i, QEMU_IOS_TOUCH_END, 0, 0);
            portable_contacts[i].active = false;
        }
        if (ios_manual_touch2) portable_emit(1, QEMU_IOS_TOUCH_END, 0, 0);
        if (ios_manual_touch) {
            InputBtnEvent btn = { .button = INPUT_BUTTON_LEFT, .down = false };
            InputEvent ev = { .type = INPUT_EVENT_KIND_BTN, .u.btn.data = &btn };
            qemu_input_event_send_impl(ios.con, &ev);
        }
        qemu_input_event_sync_impl();
    }
    ios_manual_touch = ios_manual_touch2 = false;
    if (!preserve_buttons) {
        for (int i = 0; i < 4; i++) {
            if (ios_button_holds[i].release) timer_del(ios_button_holds[i].release);
            if (ios_button_holds[i].down) ios_button_release(&ios_button_holds[i]);
        }
    }
    LIS302DLState *a = portable_accel();
    if (a) lis302dl_clear_host_motion(a);
    portable_touch_timestamp = 0;
    portable_motion_timestamp[0] = portable_motion_timestamp[1] = 0;
}
static void portable_release_all(void)
{
    portable_release_input(false);
}
static void portable_cancel_bh(void *unused) { qemu_ios_ui_cancel_input(); }
int qemu_ios_portable_cancel_input(void)
{
    if (!qemu_ios_ui_ready()) return QEMU_IOS_NOT_READY;
    return ios_input_enqueue(portable_cancel_bh, NULL, NULL)
        ? QEMU_IOS_QUEUED : QEMU_IOS_NOT_READY;
}
struct portable_touch_request {
    uint64_t id, timestamp; size_t count;
    QemuIosPortableContact contacts[QEMU_IOS_PORTABLE_MAX_CONTACTS];
};
static bool portable_touch_capacity(unsigned begins, unsigned ends, unsigned moves)
{
#if !defined(__APPLE__)
    static Object *obj;
    static bool resolved;
    portable_device(&obj, &resolved, TYPE_IPOD_TOUCH_MULTITOUCH);
    return !obj || ipod_touch_multitouch_can_queue(IPOD_TOUCH_MULTITOUCH(obj), begins, ends, moves);
#else
    return true;
#endif
}
static void portable_touch_bh(void *opaque)
{
    struct portable_touch_request *r = opaque;
    typeof(portable_contacts) next;
    int slots[QEMU_IOS_PORTABLE_MAX_CONTACTS];
    int result = QEMU_IOS_OK;
    unsigned begins = 0, ends = 0;
    unsigned moves = 0;
    memcpy(next, portable_contacts, sizeof(next));
    if (r->timestamp <= portable_touch_timestamp) result = QEMU_IOS_STALE;
    if (!ios.con) result = QEMU_IOS_NOT_READY;
    for (size_t i = 0; result == QEMU_IOS_OK && i < r->count; i++) {
        QemuIosPortableContact *c = &r->contacts[i];
        int slot = -1;
        for (int j = 0; j < 5; j++) if (next[j].active && next[j].id == c->contact_id) slot = j;
        if (c->phase == QEMU_IOS_TOUCH_BEGIN) {
            if (slot >= 0) { result = QEMU_IOS_INVALID; break; }
            for (int j = 0; j < 5; j++) if (!next[j].active) { slot = j; break; }
            if (slot < 0) { result = QEMU_IOS_UNSUPPORTED; break; }
        } else if (slot < 0) { result = QEMU_IOS_INVALID; break; }
        if (!runstate_is_running() && !runstate_check(RUN_STATE_SUSPENDED) &&
            c->phase < QEMU_IOS_TOUCH_END) { result = QEMU_IOS_NOT_READY; break; }
        slots[i] = slot;
        next[slot].active = c->phase < QEMU_IOS_TOUCH_END;
        next[slot].id = c->contact_id; next[slot].x = c->x; next[slot].y = c->y;
        begins += c->phase == QEMU_IOS_TOUCH_BEGIN;
        ends += c->phase >= QEMU_IOS_TOUCH_END;
        moves += c->phase == QEMU_IOS_TOUCH_UPDATE;
    }
    if (result == QEMU_IOS_OK) {
        ios_sequence_cancel_current();
        if (!portable_touch_capacity(begins, ends, moves)) {
            result = QEMU_IOS_BUSY;
        } else {
            for (size_t i = 0; i < r->count; i++) {
                QemuIosPortableContact *c = &r->contacts[i];
                portable_emit(slots[i], c->phase, c->x, c->y);
            }
            qemu_input_event_sync_impl();
            memcpy(portable_contacts, next, sizeof(next));
            portable_touch_timestamp = r->timestamp;
        }
    }
    qemu_mutex_lock(&ios.frame_lock);
    if (portable_status_id == r->id) portable_status = result;
    portable_pending--;
    qemu_mutex_unlock(&ios.frame_lock);
    g_free(r);
}
static void portable_touch_discard(void *opaque)
{
    struct portable_touch_request *r = opaque;
    qemu_mutex_lock(&ios.frame_lock);
    if (portable_status_id == r->id) portable_status = QEMU_IOS_NOT_READY;
    portable_pending--;
    qemu_mutex_unlock(&ios.frame_lock);
    g_free(r);
}
int qemu_ios_portable_touch(uint64_t id, uint64_t timestamp,
                           const QemuIosPortableContact *contacts, size_t count)
{
    if (!id || !timestamp || !contacts || !count || count > 8) return QEMU_IOS_INVALID;
    for (size_t i = 0; i < count; i++) {
        const QemuIosPortableContact *c = &contacts[i];
        if (c->size < sizeof(*c) || c->version != 1 || c->phase > QEMU_IOS_TOUCH_CANCEL ||
            !isfinite(c->x) || !isfinite(c->y) || c->x < 0 || c->x > 1 || c->y < 0 || c->y > 1) return QEMU_IOS_INVALID;
        for (size_t j = 0; j < i; j++) if (contacts[j].contact_id == c->contact_id) return QEMU_IOS_INVALID;
    }
    if (!qemu_ios_ui_ready()) return QEMU_IOS_NOT_READY;
    struct portable_touch_request *r = g_new0(struct portable_touch_request, 1);
    r->id = id; r->timestamp = timestamp; r->count = count;
    memcpy(r->contacts, contacts, count * sizeof(*contacts));
    ios_init_frame_lock(); qemu_mutex_lock(&ios.frame_lock);
    if (portable_pending >= 32 || id <= portable_status_id) {
        qemu_mutex_unlock(&ios.frame_lock);
        g_free(r);
        return QEMU_IOS_BUSY;
    }
    portable_pending++; portable_status_id = id; portable_status = QEMU_IOS_QUEUED;
    /* Admission and FIFO insertion are one transaction for concurrent callers.
     * The drain never holds its queue mutex while invoking a callback. */
    bool queued = ios_input_enqueue(portable_touch_bh, r, portable_touch_discard);
    if (!queued) {
        portable_pending--;
        portable_status = QEMU_IOS_NOT_READY;
    }
    qemu_mutex_unlock(&ios.frame_lock);
    if (!queued) g_free(r);
    return queued ? QEMU_IOS_QUEUED : QEMU_IOS_NOT_READY;
}
int qemu_ios_portable_input_status(uint64_t id)
{
    ios_init_frame_lock(); qemu_mutex_lock(&ios.frame_lock);
    int result = id == portable_status_id ? portable_status : QEMU_IOS_STALE;
    qemu_mutex_unlock(&ios.frame_lock); return result;
}
static void portable_motion_bh(void *opaque)
{
    QemuIosPortableMotion *m = opaque;
    LIS302DLState *a = portable_accel();
    if (a && runstate_is_running()) {
        uint32_t accepted = 0;
        for (int field = 0; field < 2; field++) {
            if ((m->fields & (1u << field)) && m->timestamp_ns > portable_motion_timestamp[field]) {
                accepted |= 1u << field;
                portable_motion_timestamp[field] = m->timestamp_ns;
            }
        }
        if (accepted) lis302dl_set_host_motion(a, accepted, m->acceleration, m->angular_velocity);
    }
    qemu_mutex_lock(&ios.frame_lock); portable_pending--; qemu_mutex_unlock(&ios.frame_lock);
    g_free(m);
}
int qemu_ios_portable_motion(const QemuIosPortableMotion *m)
{
    if (!m || m->size < sizeof(*m) || m->version != 1 || !m->timestamp_ns ||
        !m->fields || (m->fields & ~3u)) return QEMU_IOS_INVALID;
    for (int i = 0; i < 3; i++) if (!isfinite(m->acceleration[i]) || !isfinite(m->angular_velocity[i]) ||
        fabs(m->acceleration[i]) > 200 || fabs(m->angular_velocity[i]) > 100) return QEMU_IOS_INVALID;
    if (!qemu_ios_ui_ready()) return QEMU_IOS_NOT_READY;
    /* Board capability was latched on engine thread before ready publication. */
    if ((m->fields & 2) && !portable_has_gyro) return QEMU_IOS_UNSUPPORTED;
    qemu_mutex_lock(&ios.frame_lock);
    if (portable_pending >= 32) { qemu_mutex_unlock(&ios.frame_lock); return QEMU_IOS_BUSY; }
    portable_pending++; qemu_mutex_unlock(&ios.frame_lock);
    QemuIosPortableMotion *copy = g_memdup2(m, sizeof(*m));
    aio_bh_schedule_oneshot(qemu_get_aio_context(), portable_motion_bh, copy);
    return QEMU_IOS_QUEUED;
}
