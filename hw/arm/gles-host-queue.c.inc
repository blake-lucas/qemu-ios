/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * GL calls on a thread of their own (LT_GL_THREAD=off runs them on the vCPU).
 *
 * The host driver's work for a guest GL call -- validating state, building its
 * command stream, kicking the GPU -- used to run on the vCPU thread, ~15% of the
 * vCPU's time in Jetpack Joyride on a Pixel 10 and more in Epic Citadel. Here a
 * worker thread owns every host context and runs the bridge (gles_host_call_impl)
 * unchanged, and the vCPU hands it the calls:
 *
 *  - A call that answers the guest, writes guest memory or reads it in ways not
 *    known up front waits for the worker (a sync call). The vCPU is parked inside
 *    the trap meanwhile, so the bridge reads and faults guest memory through the
 *    vCPU's MMU exactly as before.
 *  - A call that only sets state (the guest shim's batchable calls, GLES_F_BATCH),
 *    a matrix load or multiply, the ES 1.1 array pointers and ES 2.0 attribute
 *    pointers, uniform arrays and the draws are queued and the guest goes on. What
 *    such a call reads of guest memory is read now, on the vCPU, and the worker's
 *    reads come from that copy (gles_guest_rw_hook): the 16 floats of a matrix, a
 *    uniform's values, the client arrays and indices of a draw. For a draw the vCPU
 *    keeps its own copy of each context's array state, taken from the bridge's at
 *    every sync call and kept up by the queued calls that change it.
 *  - A glFinish or glFlush with no call in the share group since that context's
 *    last one, and a glGetError with no call since one found no error, are answered
 *    here: backboardd does both once a frame without drawing, and the host context
 *    switch alone would flush the game's frame early.
 *
 * A queued call whose bridge code touches guest memory the vCPU did not read ahead
 * is refused there, logged, and its slot runs synchronously from then on.
 */

#define GLQ_RECS        4096
#define GLQ_ARENA       (8u << 20)
#define GLQ_MAX_PRE     12
#define GLQ_SPIN_NS     200000      /* the worker's spin before it sleeps */

typedef struct {
    uint32_t va, len, off;
} GlqRegion;

typedef struct {
    void (*fn)(void *);             /* a closure (gles_queue_run) instead of a call */
    void *opaque;
    GlesPresentPrep *present;       /* a queued present's surface pages */
    CPUState *cpu;
    uint32_t slot, ctx, argc, npre;
    uint32_t args[16];
    GlqRegion pre[GLQ_MAX_PRE];
    uint64_t arena_end;
    bool sync;
    int64_t result;
    int64_t started;                /* when the worker took it (LT_GL_THREAD_LOG) */
} GlqRec;

typedef struct {
    bool enabled, vbo;
    uint32_t size, type, stride, ptr;
} GlqArray;

typedef struct {
    GlqArray vertex, color, normal, pointsize, texcoord[GLES_MAX_TEXUNITS], attr[GLES_MAX_ATTRIBS];
    uint32_t unit;
    bool array_bound, element_bound, program;
    /*
     * The share group's IOSurface textures (added only by a sync call). A draw re-reads a
     * surface whose memory changed, first checking through the vCPU's MMU that the memory
     * is still the surface's, so while there are any, draws wait.
     */
    GHashTable *surfaces;
    /*
     * Calls since a glGetError found no error, and the share group's call count when this
     * context last finished: surfaces belong to the group, so any of its contexts' calls
     * can leave one for this context's finish to write back.
     */
    bool error_unknown;
    uint64_t *group_calls, finished_at;
} GlqMirror;

static struct {
    int on;
    bool log;                       /* LT_GL_THREAD_LOG: counts every 10 s */
    QemuThread thread;
    QemuMutex lock;
    QemuCond work, done;
    GlqRec rec[GLQ_RECS];
    uint8_t *arena;
    uint64_t head, tail;            /* records queued, records finished */
    uint64_t ahead, atail;          /* arena bytes handed out, released */
    bool worker_sleeping, vcpu_waiting;
    GHashTable *mirrors;            /* ctx -> GlqMirror, vCPU only */
    GHashTable *groups;             /* GLESGroup * -> call count, vCPU only */
    uint8_t sync_slot[GLES_OP_BASE];  /* slots found to need the vCPU */
    const GlqRec *running;          /* the worker's queued call */
    uint64_t async, sync, misses;
    /* LT_GL_THREAD_LOG: per slot, the waits' count, time behind queued calls and own time */
    uint64_t wait_n[GLES_OP_BASE + 16], wait_behind[GLES_OP_BASE + 16], wait_own[GLES_OP_BASE + 16];
} glq = { .on = -1 };

static const uint8_t glq_flags[GLES_ID_MAX + 1] = {
#define GLES_FN(n, f, id, argc, fl) [id] = (fl),
#include "hw/arm/guest-services/gles-names.h"
#undef GLES_FN
};

/* ------------------------------------------------------------------ worker */

static int glq_payload_rw(vaddr va, void *buf, size_t len, bool write)
{
    const GlqRec *r = glq.running;

    if (!write) {
        for (uint32_t i = 0; i < r->npre; i++) {
            const GlqRegion *g = &r->pre[i];

            if (va >= g->va && va + len <= (uint64_t)g->va + g->len) {
                memcpy(buf, glq.arena + g->off + (va - g->va), len);
                return 0;
            }
        }
    }
    if (r->slot < GLES_OP_BASE) {
        qatomic_set(&glq.sync_slot[r->slot], 1);
    } else {
        gles_present_async_no_queue();
    }
    glq.misses++;
    fprintf(stderr, "[glq] queued %s (slot %u) %s guest 0x%08" PRIx64 "+%zu not read ahead; "
            "the slot waits from now on\n", gles_id_name(r->slot), r->slot,
            write ? "writes" : "reads", (uint64_t)va, len);
    return -1;
}

static void glq_execute(GlqRec *r)
{
    if (r->fn) {
        r->fn(r->opaque);
        return;
    }
    if (r->sync && glq.log) {
        r->started = get_clock();
    }
    if (!r->sync) {
        glq.running = r;
        gles_guest_rw_hook = glq_payload_rw;
    }
    if (r->present) {
        gles_present_async_run(r->present, true);
    }
    r->result = gles_host_call_impl(r->cpu, r->slot, r->ctx, r->argc, r->args);
    if (r->present) {
        gles_present_async_run(r->present, false);
        g_free(r->present);
    }
    gles_guest_rw_hook = NULL;
    glq.running = NULL;
}

static void *glq_worker(void *opaque)
{
    rcu_register_thread();
    for (;;) {
        uint64_t t = glq.tail;

        if (qatomic_load_acquire(&glq.head) == t) {
            int64_t until = get_clock() + GLQ_SPIN_NS;

            while (qatomic_load_acquire(&glq.head) == t && get_clock() < until) {
                cpu_relax();
            }
            if (qatomic_load_acquire(&glq.head) == t) {
                qemu_mutex_lock(&glq.lock);
                qatomic_set(&glq.worker_sleeping, true);
                smp_mb();
                while (qatomic_load_acquire(&glq.head) == t) {
                    qemu_cond_wait(&glq.work, &glq.lock);
                }
                qatomic_set(&glq.worker_sleeping, false);
                qemu_mutex_unlock(&glq.lock);
            }
        }
        GlqRec *r = &glq.rec[t % GLQ_RECS];

        glq_execute(r);
        qatomic_store_release(&glq.atail, r->arena_end);
        qatomic_store_release(&glq.tail, t + 1);
        smp_mb();
        if (qatomic_read(&glq.vcpu_waiting)) {
            qemu_mutex_lock(&glq.lock);
            qemu_cond_broadcast(&glq.done);
            qemu_mutex_unlock(&glq.lock);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------- vCPU */

/* Until cond holds: spin a little (the worker is usually just behind), then sleep. */
#define GLQ_WAIT(cond)                                                          \
    do {                                                                        \
        if (cond) {                                                             \
            break;                                                              \
        }                                                                       \
        int64_t until_ = get_clock() + 2000000;                                 \
        while (!(cond) && get_clock() < until_) {                               \
            cpu_relax();                                                        \
        }                                                                       \
        if (!(cond)) {                                                          \
            qemu_mutex_lock(&glq.lock);                                         \
            qatomic_set(&glq.vcpu_waiting, true);                               \
            smp_mb();                                                           \
            while (!(cond)) {                                                   \
                qemu_cond_wait(&glq.done, &glq.lock);                           \
            }                                                                   \
            qatomic_set(&glq.vcpu_waiting, false);                              \
            qemu_mutex_unlock(&glq.lock);                                       \
        }                                                                       \
    } while (0)

static bool glq_enabled(void)
{
    if (glq.on < 0) {
        const char *v = getenv("LT_GL_THREAD");

        glq.on = !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
        glq.log = getenv("LT_GL_THREAD_LOG") != NULL;
        if (glq.on) {
            qemu_mutex_init(&glq.lock);
            qemu_cond_init(&glq.work);
            qemu_cond_init(&glq.done);
            glq.arena = g_malloc(GLQ_ARENA);
            glq.mirrors = g_hash_table_new_full(NULL, NULL, NULL, g_free);
            glq.groups = g_hash_table_new_full(NULL, NULL, NULL, g_free);
            qemu_thread_create(&glq.thread, "gles-gl", glq_worker, NULL, QEMU_THREAD_DETACHED);
        }
    }
    return glq.on;
}

/* The next free record, its payload space reserved: `bytes` of arena at *off. */
static GlqRec *glq_alloc(uint32_t bytes, uint32_t *off)
{
    uint64_t h = glq.head;
    uint64_t pos = glq.ahead % GLQ_ARENA, skip = pos + bytes > GLQ_ARENA ? GLQ_ARENA - pos : 0;

    GLQ_WAIT(h - qatomic_load_acquire(&glq.tail) < GLQ_RECS &&
             glq.ahead + skip + bytes - qatomic_load_acquire(&glq.atail) <= GLQ_ARENA);
    glq.ahead += skip;
    *off = glq.ahead % GLQ_ARENA;
    glq.ahead += bytes;
    GlqRec *r = &glq.rec[h % GLQ_RECS];
    memset(r, 0, sizeof(*r));
    r->arena_end = glq.ahead;
    return r;
}

static void glq_commit(GlqRec *r)
{
    qatomic_store_release(&glq.head, glq.head + 1);
    smp_mb();
    if (qatomic_read(&glq.worker_sleeping)) {
        qemu_mutex_lock(&glq.lock);
        qemu_cond_signal(&glq.work);
        qemu_mutex_unlock(&glq.lock);
    }
}

static void glq_wait_idle(void)
{
    uint64_t h = glq.head;

    GLQ_WAIT(qatomic_load_acquire(&glq.tail) == h);
}

/* Run fn on the worker, which holds the host contexts, and wait for it. */
static void gles_queue_run(void (*fn)(void *), void *opaque)
{
    uint32_t off;

    if (!glq_enabled() || qemu_thread_is_self(&glq.thread)) {
        fn(opaque);
        return;
    }
    GlqRec *r = glq_alloc(0, &off);
    r->fn = fn;
    r->opaque = opaque;
    r->sync = true;
    glq_commit(r);
    glq_wait_idle();
}

static GlqMirror *glq_mirror(uint32_t ctx)
{
    GlqMirror *m = g_hash_table_lookup(glq.mirrors, GUINT_TO_POINTER(ctx));

    if (!m) {
        m = g_new0(GlqMirror, 1);
        g_hash_table_insert(glq.mirrors, GUINT_TO_POINTER(ctx), m);
    }
    return m;
}

static void glq_mirror_array(GlqArray *m, const GLESArray *a)
{
    m->enabled = a->enabled;
    m->vbo = a->vbo != NULL;
    m->size = a->size;
    m->type = a->type;
    m->stride = a->stride;
    m->ptr = a->ptr;
}

/* After a sync call, with the worker idle: the bridge's array state is the truth. */
static void glq_mirror_refresh(uint32_t ctx)
{
    GLESHost *h = gles_contexts ? g_hash_table_lookup(gles_contexts, GUINT_TO_POINTER(ctx)) : NULL;
    GlqMirror *m;

    if (!h && ctx >= 0x80000000) {
        g_hash_table_remove(glq.mirrors, GUINT_TO_POINTER(ctx));
        return;
    }
    h = h ? h : &gh_legacy;
    m = glq_mirror(ctx);
    m->surfaces = h->surfaces;
    glq_mirror_array(&m->vertex, &h->vertex);
    glq_mirror_array(&m->color, &h->color);
    glq_mirror_array(&m->normal, &h->normal);
    glq_mirror_array(&m->pointsize, &h->pointsize);
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) {
        glq_mirror_array(&m->texcoord[i], &h->texcoord[i]);
    }
    for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
        glq_mirror_array(&m->attr[i], &h->attr[i]);
    }
    m->unit = h->client_active_unit;
    m->array_bound = h->array_buffer != NULL;
    m->element_bound = h->element_buffer != NULL;
    m->program = h->program != 0;
    m->group_calls = g_hash_table_lookup(glq.groups, h->group);
    if (!m->group_calls) {
        m->group_calls = g_new0(uint64_t, 1);
        g_hash_table_insert(glq.groups, h->group, m->group_calls);
    }
}

/* After a call that may have freed other contexts (a process's exit, gles_owner_free), with the worker idle: their
 * mirrors go, and the call counters of share groups no mirror names any more (a later group may reuse the address). */
static void glq_prune(void)
{
    g_autoptr(GHashTable) used = g_hash_table_new(NULL, NULL);
    GHashTableIter it;
    gpointer key, value;

    g_hash_table_iter_init(&it, glq.mirrors);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        uint32_t ctx = GPOINTER_TO_UINT(key);

        if (ctx >= 0x80000000 && !(gles_contexts && g_hash_table_contains(gles_contexts, key))) {
            g_hash_table_iter_remove(&it);
        } else {
            g_hash_table_add(used, ((GlqMirror *)value)->group_calls);
        }
    }
    g_hash_table_iter_init(&it, glq.groups);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        if (!g_hash_table_contains(used, value)) g_hash_table_iter_remove(&it);
    }
}

typedef struct {
    uint32_t n;
    GlqRegion r[GLQ_MAX_PRE];
    uint32_t bytes;
} GlqPrefetch;

static bool glq_want(GlqPrefetch *p, uint32_t va, uint32_t len)
{
    if (!len) {
        return true;
    }
    if (p->n == GLQ_MAX_PRE || (uint64_t)va + len > UINT32_MAX || len > GLQ_ARENA / 4) {
        return false;
    }
    p->r[p->n++] = (GlqRegion){ va, len, p->bytes };
    p->bytes += ROUND_UP(len, 16);
    return true;
}

/* An ES 1.1 array as gles_bind_array reads it for elements [first, first + count). */
static bool glq_want_array(GlqPrefetch *p, const GlqArray *a, uint32_t first, uint32_t count)
{
    uint32_t esz = gles_type_size(a->type) * a->size, stride;

    if (!a->enabled || a->vbo || !a->ptr || !count || !esz) {
        return true;
    }
    stride = a->stride ? a->stride : esz;
    return glq_want(p, a->ptr + stride * first, stride * (count - 1) + esz);
}

static bool glq_want_draw(GlqPrefetch *p, const GlqMirror *m, uint32_t mode, uint32_t first,
                          uint32_t count)
{
    if (m->program) {                   /* gles_es2_bind_attr */
        for (unsigned i = 0; i < GLES_MAX_ATTRIBS; i++) {
            if (!glq_want_array(p, &m->attr[i], first, count)) {
                return false;
            }
        }
        return true;
    }
    if (!glq_want_array(p, &m->vertex, first, count) ||
        !glq_want_array(p, &m->color, first, count) ||
        !glq_want_array(p, &m->normal, first, count)) {
        return false;
    }
    for (unsigned i = 0; i < GLES_MAX_TEXUNITS; i++) {
        if (!glq_want_array(p, &m->texcoord[i], first, count)) {
            return false;
        }
    }
    if (mode == GL_POINTS && m->pointsize.enabled && !m->pointsize.vbo && m->pointsize.ptr) {
        uint32_t stride = m->pointsize.stride ? m->pointsize.stride : 4;   /* gles_draw_sized_points */

        return glq_want(p, m->pointsize.ptr + stride * first, stride * count);
    }
    return true;
}

/* The client-array pointer calls and client state, as the bridge applies them. */
static void glq_mirror_note(GlqMirror *m, uint32_t slot, const uint32_t *a)
{
    GlqArray *x = NULL;

    switch (slot) {
    case GLES_SLOT_ENABLE_CLIENT_STATE:
    case GLES_SLOT_DISABLE_CLIENT_STATE: {
        bool on = slot == GLES_SLOT_ENABLE_CLIENT_STATE;

        switch (a[0]) {
        case GL_VERTEX_ARRAY:           m->vertex.enabled = on; break;
        case GL_TEXTURE_COORD_ARRAY:    m->texcoord[m->unit].enabled = on; break;
        case GL_COLOR_ARRAY:            m->color.enabled = on; break;
        case GL_NORMAL_ARRAY:           m->normal.enabled = on; break;
        case GL_POINT_SIZE_ARRAY_OES:   m->pointsize.enabled = on; break;
        }
        return;
    }
    case GLES_SLOT_VERTEX_POINTER:      x = &m->vertex; break;
    case GLES_SLOT_COLOR_POINTER:       x = &m->color; break;
    case GLES_SLOT_TEXCOORD_POINTER:    x = &m->texcoord[m->unit]; break;
    case GLES_SLOT_NORMAL_POINTER:
        m->normal = (GlqArray){ m->normal.enabled, m->array_bound, 3, a[0], a[1], a[2] };
        return;
    case GLES_SLOT_POINT_SIZE_POINTER_OES:
        m->pointsize = (GlqArray){ m->pointsize.enabled, m->array_bound, 1, a[0], a[1], a[2] };
        return;
    case GLES_SLOT_CLIENT_ACTIVE_TEXTURE:
        if (a[0] >= GL_TEXTURE0 && a[0] < GL_TEXTURE0 + GLES_MAX_TEXUNITS) {
            m->unit = a[0] - GL_TEXTURE0;
        }
        return;
    case GLES_ID_glBindBuffer:
        if (a[0] == GLES_ELEMENT_ARRAY_BUFFER) {
            m->element_bound = a[1] != 0;
        } else {
            m->array_bound = a[1] != 0;
        }
        return;
    case GLES_ID_glUseProgram:
        m->program = a[0] != 0;
        return;
    case GLES_ID_glVertexAttribPointer:     /* index, size, type, normalized, stride, ptr */
        if (a[0] < GLES_MAX_ATTRIBS) {
            GlqArray *t = &m->attr[a[0]];

            *t = (GlqArray){ t->enabled, m->array_bound, a[1], a[2], a[4], a[5] };
        }
        return;
    case GLES_ID_glEnableVertexAttribArray:
    case GLES_ID_glDisableVertexAttribArray:
        if (a[0] < GLES_MAX_ATTRIBS) {
            m->attr[a[0]].enabled = slot == GLES_ID_glEnableVertexAttribArray;
        }
        return;
    default:
        return;
    }
    *x = (GlqArray){ x->enabled, m->array_bound, a[0], a[1], a[2], a[3] };
}

static bool glq_surfaces(const GlqMirror *m)
{
    return m->surfaces && g_hash_table_size(m->surfaces);
}

/*
 * What a queued call reads of guest memory, or false if it has to wait: not known
 * up front, a draw this does not model (ES 2.0 programs, indices in a buffer with
 * arrays in guest memory), or a draw that may re-read an IOSurface.
 */
static bool glq_plan(CPUState *cpu, GlqMirror *m, uint32_t slot, uint32_t argc, const uint32_t *a,
                     GlqPrefetch *p, uint8_t **idx)
{
    switch (slot) {
    case GLES_SLOT_LOAD_MATRIXF:
    case GLES_SLOT_MULT_MATRIXF:
    case GLES_SLOT_LOAD_MATRIXX:
    case GLES_SLOT_MULT_MATRIXX:
        return a[0] && glq_want(p, a[0], 64);
    case GLES_SLOT_VERTEX_POINTER:
    case GLES_SLOT_COLOR_POINTER:
    case GLES_SLOT_TEXCOORD_POINTER:
    case GLES_SLOT_NORMAL_POINTER:
    case GLES_SLOT_POINT_SIZE_POINTER_OES:
        return true;
    case GLES_ID_glVertexAttribPointer:
        return a[0] < GLES_MAX_ATTRIBS;
    case GLES_ID_glUniform1fv ... GLES_ID_glUniform4iv: {    /* location, count, value: gles_es2_fetch */
        static const uint8_t vec_n[] = { 1, 2, 3, 4 };
        uint64_t n = (uint64_t)a[1] * vec_n[(slot - GLES_ID_glUniform1fv) % 4];

        return a[2] && n && n <= 65536 && glq_want(p, a[2], n * 4);
    }
    case GLES_ID_glUniformMatrix2fv ... GLES_ID_glUniformMatrix4fv: {   /* location, count, transpose, value */
        uint64_t n = (uint64_t)a[1] * (slot == GLES_ID_glUniformMatrix2fv ? 4 :
                                       slot == GLES_ID_glUniformMatrix3fv ? 9 : 16);

        return a[3] && n && n <= 65536 && glq_want(p, a[3], n * 4);
    }
    case GLES_SLOT_DRAW_ARRAYS:
        if (glq_surfaces(m) || (m->program && !a[2])) {
            return false;
        }
        return glq_want_draw(p, m, a[0], a[1], a[2]);
    case GLES_SLOT_DRAW_ELEMENTS: {
        uint32_t count = a[1], itype = a[2], iptr = a[3], isz = gles_type_size(itype);
        uint32_t lo = UINT32_MAX, hi = 0;

        if (glq_surfaces(m) || (itype != GL_UNSIGNED_BYTE && itype != GL_UNSIGNED_SHORT) || !count) {
            return false;
        }
        if (m->element_bound) {
            /* The indices are in a buffer only the worker reads: fine if no array needs fetching. */
            GlqPrefetch none = { 0 };

            return glq_want_draw(&none, m, a[0], 0, 1) && none.n == 0;
        }
        if (!iptr || !glq_want(p, iptr, count * isz)) {
            return false;
        }
        *idx = g_malloc(count * isz);
        if (gles_guest_rw(cpu, iptr, *idx, count * isz, 0)) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = isz == 1 ? (*idx)[i] : lduw_le_p(*idx + i * 2);

            lo = MIN(lo, v);
            hi = MAX(hi, v);
        }
        if (!gles_index_range) {
            lo = 0;
        }
        return glq_want_draw(p, m, a[0], lo, hi - lo + 1);
    }
    default:
        return slot < GLES_ID_MAX + 1 && (glq_flags[slot] & GLES_F_BATCH) &&
               slot != GLES_ID_glBindFramebuffer;
    }
}

/* The slots the vCPU waited on longest since the last report: count, µs behind queued calls, µs own. */
static void glq_log_waits(void)
{
    for (int k = 0; k < 8; k++) {
        uint32_t best = 0;

        for (uint32_t i = 1; i < ARRAY_SIZE(glq.wait_n); i++) {
            if (glq.wait_behind[i] + glq.wait_own[i] > glq.wait_behind[best] + glq.wait_own[best]) {
                best = i;
            }
        }
        if (!glq.wait_n[best]) {
            break;
        }
        char op[16];

        snprintf(op, sizeof(op), "op 0x%x", best);
        fprintf(stderr, "[glq]   %-28s %7" PRIu64 " waits, behind %7" PRIu64 " us, own %7" PRIu64 " us\n",
                best >= GLES_OP_BASE ? op : gles_id_name(best), glq.wait_n[best],
                glq.wait_behind[best] / 1000, glq.wait_own[best] / 1000);
        glq.wait_n[best] = glq.wait_behind[best] = glq.wait_own[best] = 0;
    }
    memset(glq.wait_n, 0, sizeof(glq.wait_n));
    memset(glq.wait_behind, 0, sizeof(glq.wait_behind));
    memset(glq.wait_own, 0, sizeof(glq.wait_own));
}

/* A call from the vCPU: queued, or run on the worker while the vCPU waits. */
static int64_t gles_queue_call(CPUState *cpu, uint32_t slot, uint32_t ctx, uint32_t argc,
                               const uint32_t *a)
{
    GlqPrefetch p = { 0 };
    g_autofree uint8_t *idx = NULL;
    /* A context's first call waits, so that its mirror starts from the bridge's state. */
    GlqMirror *m = g_hash_table_lookup(glq.mirrors, GUINT_TO_POINTER(ctx));
    uint32_t off;
    GlqRec *r;

    gles_read_switches();
    if (m) {
        /*
         * No call since the last finish (or since a glGetError found no error): nothing
         * to wait for or write back, and no error. backboardd finishes its context and
         * checks for errors once a frame without drawing.
         */
        if (slot == GLES_SLOT_GET_ERROR) {
            if (!m->error_unknown) {
                return GL_NO_ERROR;
            }
        } else if (slot == GLES_SLOT_FINISH || slot == GLES_SLOT_FLUSH) {
            if (*m->group_calls == m->finished_at) {
                return 0;
            }
        } else {
            ++*m->group_calls;
            m->error_unknown = true;
        }
    }
    if (slot == GLES_OP_PRESENT_SURFACE && glq.log) {
        static int64_t next;
        int64_t now = get_clock();

        if (now >= next) {
            next = now + 10 * NANOSECONDS_PER_SECOND;
            fprintf(stderr, "[glq] queued %" PRIu64 " waited %" PRIu64 " misses %" PRIu64 "\n",
                    glq.async, glq.sync, glq.misses);
            glq_log_waits();
        }
    }
    if (m && slot == GLES_OP_PRESENT_SURFACE && argc >= 5 && gles_allowed) {
        /* The frame's calls may still be running: the GL thread presents it when it gets there. */
        GlesPresentPrep *prep = gles_present_async_prepare(cpu, ctx, a);

        if (prep) {
            r = glq_alloc(0, &off);
            r->cpu = cpu;
            r->slot = slot;
            r->ctx = ctx;
            r->argc = argc;
            memcpy(r->args, a, argc * sizeof(uint32_t));
            r->present = prep;
            glq_commit(r);
            glq.async++;
            return 0;
        }
    }
    if (m && slot < GLES_OP_BASE && !qatomic_read(&glq.sync_slot[slot]) && gles_allowed &&
        glq_plan(cpu, m, slot, argc, a, &p, &idx)) {
        uint64_t ahead = glq.ahead;

        r = glq_alloc(p.bytes, &off);
        for (uint32_t i = 0; i < p.n; i++) {
            /* The indices were read already; the rest is read now, as the guest has them. */
            uint8_t *dst = glq.arena + off + p.r[i].off;

            if (idx && i == 0 && slot == GLES_SLOT_DRAW_ELEMENTS) {
                memcpy(dst, idx, p.r[i].len);
            } else if (gles_guest_rw(cpu, p.r[i].va, dst, p.r[i].len, 0)) {
                /* A page to fault in: nothing is queued, the guest reissues the call. */
                glq.ahead = ahead;
                return -1;
            }
            p.r[i].off += off;
        }
        r->cpu = cpu;
        r->slot = slot;
        r->ctx = ctx;
        r->argc = argc;
        memcpy(r->args, a, argc * sizeof(uint32_t));
        r->npre = p.n;
        memcpy(r->pre, p.r, p.n * sizeof(GlqRegion));
        glq_mirror_note(m, slot, a);
        glq_commit(r);
        glq.async++;
        return 0;
    }
    r = glq_alloc(0, &off);
    r->cpu = cpu;
    r->slot = slot;
    r->ctx = ctx;
    r->argc = argc;
    memcpy(r->args, a, argc * sizeof(uint32_t));
    r->sync = true;
    int64_t t0 = glq.log ? get_clock() : 0;
    glq_commit(r);
    glq_wait_idle();
    glq.sync++;
    if (glq.log && slot < ARRAY_SIZE(glq.wait_n)) {
        int64_t t1 = get_clock();

        glq.wait_n[slot]++;
        glq.wait_behind[slot] += r->started - t0;
        glq.wait_own[slot] += t1 - r->started;
    }
    glq_mirror_refresh(ctx);
    if (slot == GLES_OP_NEW_SHAREGROUP || slot == GLES_OP_DELETE_SHAREGROUP) {
        glq_prune();
    }
    m = g_hash_table_lookup(glq.mirrors, GUINT_TO_POINTER(ctx));
    if (m && (slot == GLES_SLOT_FINISH || slot == GLES_SLOT_FLUSH) && r->result >= 0) {
        m->finished_at = *m->group_calls;
    } else if (m && slot == GLES_SLOT_GET_ERROR && r->result == GL_NO_ERROR) {
        m->error_unknown = false;
    } else if (m && slot != GLES_SLOT_GET_ERROR && slot != GLES_SLOT_FINISH && slot != GLES_SLOT_FLUSH) {
        ++*m->group_calls;                          /* a first call: a new mirror */
        m->error_unknown = true;
    }
    return r->result;
}

/* The contexts were dropped or replaced: the mirrors are rebuilt as calls come. */
static void gles_queue_forget(void)
{
    if (glq.on > 0) {
        g_hash_table_remove_all(glq.mirrors);
        g_hash_table_remove_all(glq.groups);
    }
}
