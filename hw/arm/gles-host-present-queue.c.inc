/*
 * The present queued on the GL thread where the frame is read back (desktop Linux; LT_PRESENT_ASYNC=off for A/B).
 *
 * Without an AHardwareBuffer (gles-host-present-async.c.inc) a present is glFinish, glReadPixels and a copy of the
 * frame into the guest's surface, and the vCPU waited for all of it inside the GL thread's sync call: in Epic Citadel
 * on an iPhone 5, ~5 ms a frame on a desktop GPU, ~15% of the first AP core's time. Here the vCPU makes the present's
 * checks and resolves the surface's pages through its TLB as a guest store would (plain RAM only, no TLB flags), and
 * queues it; the GL thread reads the frame back when it gets there and copies it into those pages. Calls behind it in
 * the queue run after the copy, so the context's next frame can't reach the readback.
 *
 * Presents are numbered as on Android: `issued` on the vCPU, `done` once the frame is in guest memory or was given up.
 * Who reads frames from outside the GL thread waits for done to reach issued (gles_present_wait_hook): the scaler
 * before it reads a source, the display before it latches, and a stopping machine before its RAM is saved. A guest
 * CPU read of the surface right after its present could see the previous frame there; nothing in iOS's display path
 * reads it that way (CoreAnimation hands out three surfaces in turn).
 */
#include "hw/arm/gles-present.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/target_page.h"
#include "system/runstate.h"

static GHashTable *gles_contexts;      /* ctx -> GLESHost (defined with the context ops) */

/* A queued present's surface pages, resolved by the vCPU. */
typedef struct GlesPresentPrep {
    uint64_t seq;
    uint32_t base;
    size_t npages;
    uint8_t *pages[];
} GlesPresentPrep;

static struct {
    int state;                  /* -1 unknown, 0 off, 1 ready */
    bool queue;                 /* presents may be queued (cleared if the GL thread refuses one) */
    QemuMutex lock;
    QemuCond cond;
    uint64_t issued;            /* presents numbered (vCPU) */
    uint64_t done;              /* presents in guest memory or given up (written under the lock) */
} gpq = { .state = -1 };

/* The queued present the GL thread is running, if it is running one. */
static __thread GlesPresentPrep *gpq_prep;

static void gles_present_queue_mark_done(uint64_t seq)
{
    qemu_mutex_lock(&gpq.lock);
    if (gpq.done < seq) {
        qatomic_store_release(&gpq.done, seq);
    }
    qemu_cond_broadcast(&gpq.cond);
    qemu_mutex_unlock(&gpq.lock);
}

static void gles_present_queue_settle_to(uint64_t seq)
{
    if (qatomic_load_acquire(&gpq.done) >= seq) {
        return;
    }
    qemu_mutex_lock(&gpq.lock);
    while (gpq.done < seq) {
        qemu_cond_wait(&gpq.cond, &gpq.lock);
    }
    qemu_mutex_unlock(&gpq.lock);
}

/*
 * A device about to read frames (the scaler, the display): until every present handed out is in guest memory. The
 * GL thread never takes the BQL, but the display waits with it held, so it gives up after a while rather than trust
 * that forever.
 */
static void gles_present_queue_hook(void)
{
    uint64_t seq = qatomic_read(&gpq.issued);

    if (qatomic_load_acquire(&gpq.done) >= seq) {
        return;
    }
    if (!bql_locked()) {
        gles_present_queue_settle_to(seq);
        return;
    }
    qemu_mutex_lock(&gpq.lock);
    while (gpq.done < seq) {
        if (!qemu_cond_timedwait(&gpq.cond, &gpq.lock, 200)) {
            static bool told;

            if (!told) {
                told = true;
                fprintf(stderr, "[gles] present %" PRIu64 " not in guest memory after 200 ms; "
                        "the display goes on without it\n", seq);
            }
            break;
        }
    }
    qemu_mutex_unlock(&gpq.lock);
}

/* A stopped machine's RAM is about to be saved or read. */
static void gles_present_queue_vm_state(void *opaque, bool running, RunState state)
{
    if (!running) {
        gles_present_queue_settle_to(qatomic_read(&gpq.issued));
    }
}

static bool gles_present_queue_ready(void)
{
    static gsize once;

    /* Both AP cores make GL calls. */
    if (g_once_init_enter(&once)) {
        const char *v = getenv("LT_PRESENT_ASYNC");

        if ((v && (!strcmp(v, "off") || !strcmp(v, "0"))) || getenv("IT_GLES_FRAME_METRICS") ||
            getenv("IT_GLES_VERBOSE") || getenv("IT_GLES_DUMP_DIR") || gles_prof || getenv("IT_GLES_PANEL_ALSO") ||
            getenv("LT_GLES_SURFACE_PATH") || getenv("IT_GLES_PRESENT_TEST")) {
            gpq.state = 0;          /* those read or replace the frame on the vCPU */
        } else {
            qemu_mutex_init(&gpq.lock);
            qemu_cond_init(&gpq.cond);
            gpq.queue = true;
            qatomic_set(&gles_present_wait_hook, gles_present_queue_hook);
            qemu_add_vm_change_state_handler(gles_present_queue_vm_state, NULL);
            gpq.state = 1;
        }
        g_once_init_leave(&once, 1);
    }
    return gpq.state > 0;
}

/* The AHardwareBuffer path's: none here. */
static inline bool gles_present_async_submit(CPUState *cpu, uint32_t base, uint32_t stride,
                                             uint32_t rw, uint32_t rh)
{
    return false;
}

static inline void gles_present_async_before_call(uint32_t ctx)
{
}

/*
 * On the vCPU: a present the GL thread can run later, or NULL to run it now. The checks are the readback path's own
 * on the context's state, which only calls the vCPU waits for change; the surface's pages are resolved through the
 * vCPU's TLB as a guest store would.
 */
static GlesPresentPrep *gles_present_async_prepare(CPUState *cpu, uint32_t ctx, const uint32_t *a)
{
    GLESHost *h = gles_contexts ? g_hash_table_lookup(gles_contexts, GUINT_TO_POINTER(ctx)) : NULL;
    uint32_t base = a[0], stride = a[1], w = a[2], rh = a[3];
    uint64_t first = base & TARGET_PAGE_MASK, end = (uint64_t)base + (uint64_t)(rh - 1) * stride + w * 4;
    CPUArchState *env = cpu_env(cpu);
    int idx = cpu_mmu_index(cpu, false);
    GlesPresentPrep *prep;
    size_t npages;

    if (!gles_present_queue_ready() || !qatomic_read(&gpq.queue) || !h || h->iosurface || gles_swizzle ||
        a[4] != GLES_SURFACE_BGRA32 || !base || !w || !rh || !h->drawable_announced || w != h->drawable_width ||
        rh != h->drawable_height || stride < w * 4 || end > UINT32_MAX) {
        return NULL;
    }
    npages = (end - first + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS;
    prep = g_malloc(sizeof(*prep) + npages * sizeof(prep->pages[0]));
    prep->npages = npages;
    prep->base = base;
    for (size_t i = 0; i < npages; i++) {
        void *host;

        if (probe_access_flags(env, first + i * TARGET_PAGE_SIZE, 0, MMU_DATA_STORE, idx, true,
                               &host, 0) || !host) {
            g_free(prep);
            return NULL;
        }
        prep->pages[i] = host;
    }
    prep->seq = qatomic_read(&gpq.issued) + 1;
    qatomic_set(&gpq.issued, prep->seq);
    return prep;
}

/*
 * The readback path's row copy into the guest's surface: into the queued present's pages, or through the vCPU's MMU
 * (gles_guest_rw) when the present runs on the vCPU's turn. Row @y of the surface at guest @base.
 */
static int gles_present_write_row(CPUState *cpu, uint32_t base, uint32_t y, uint32_t stride, const uint8_t *src,
                                  size_t len)
{
    GlesPresentPrep *prep = gpq_prep;

    if (prep) {
        uint64_t off = (base & ~TARGET_PAGE_MASK) + (uint64_t)y * stride;

        if (base != prep->base || ((off + len + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS) > prep->npages) {
            return -1;
        }
        for (size_t done = 0; done < len;) {
            uint64_t at = off + done;
            size_t in = at & ~TARGET_PAGE_MASK, n = MIN(len - done, TARGET_PAGE_SIZE - in);

            memcpy(prep->pages[at >> TARGET_PAGE_BITS] + in, src + done, n);
            done += n;
        }
        return 0;
    }
    return gles_guest_rw(cpu, base + (hwaddr)y * stride, (void *)src, len, 1);
}

/* On the GL thread, around a queued present: done once it returns, copied or given up. */
static void gles_present_async_run(GlesPresentPrep *prep, bool begin)
{
    if (begin) {
        gpq_prep = prep;
        return;
    }
    gpq_prep = NULL;
    gles_present_queue_mark_done(prep->seq);
}

/* The GL thread refused a queued present's guest access: present on the vCPU's turn from now on. */
static void gles_present_async_no_queue(void)
{
    qatomic_set(&gpq.queue, false);
}
