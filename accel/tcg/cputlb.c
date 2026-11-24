/*
 *  Common CPU TLB handling
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/target-info.h"
#include "accel/tcg/cpu-ops.h"
#include "accel/tcg/iommu.h"
#include "accel/tcg/probe.h"
#include "exec/page-protection.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "accel/tcg/cpu-ldst-common.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "system/ramblock.h"
#include "exec/mmu-access-type.h"
#include "exec/tlb-common.h"
#include "exec/vaddr.h"
#include "tcg/tcg.h"
#include "qemu/error-report.h"
#include "exec/log.h"
#include "exec/helper-proto-common.h"
#include "exec/tlb-flags.h"
#include "qemu/atomic.h"
#include "qemu/atomic128.h"
#include "tb-internal.h"
#include "trace.h"
#include "tb-hash.h"
#include "tb-internal.h"
#include "tlb-bounds.h"
#include "internal-common.h"
#ifdef CONFIG_PLUGIN
#include "qemu/plugin-memory.h"
#endif
#include "tcg/tcg-ldst.h"
#include "backend-ldst.h"


/* DEBUG defines, enable DEBUG_TLB_LOG to log to the CPU_LOG_MMU target */
/* #define DEBUG_TLB */
/* #define DEBUG_TLB_LOG */

#ifdef DEBUG_TLB
# define DEBUG_TLB_GATE 1
# ifdef DEBUG_TLB_LOG
#  define DEBUG_TLB_LOG_GATE 1
# else
#  define DEBUG_TLB_LOG_GATE 0
# endif
#else
# define DEBUG_TLB_GATE 0
# define DEBUG_TLB_LOG_GATE 0
#endif

#define tlb_debug(fmt, ...) do { \
    if (DEBUG_TLB_LOG_GATE) { \
        qemu_log_mask(CPU_LOG_MMU, "%s: " fmt, __func__, \
                      ## __VA_ARGS__); \
    } else if (DEBUG_TLB_GATE) { \
        fprintf(stderr, "%s: " fmt, __func__, ## __VA_ARGS__); \
    } \
} while (0)

#define assert_cpu_is_self(cpu) do {                              \
        if (DEBUG_TLB_GATE) {                                     \
            g_assert(!(cpu)->created || qemu_cpu_is_self(cpu));   \
        }                                                         \
    } while (0)

/* run_on_cpu_data.target_ptr should always be big enough for a
 * vaddr even on 32 bit builds
 */
QEMU_BUILD_BUG_ON(sizeof(vaddr) > sizeof(run_on_cpu_data));

#define ALL_MMUIDX_BITS ((1 << NB_MMU_MODES) - 1)

static inline size_t tlb_n_entries(CPUTLBDescFast *fast)
{
    return (fast->mask >> CPU_TLB_ENTRY_BITS) + 1;
}

static inline size_t sizeof_tlb(CPUTLBDescFast *fast)
{
    return fast->mask + (1 << CPU_TLB_ENTRY_BITS);
}

static inline uint64_t tlb_read_idx(const CPUTLBEntry *entry,
                                    MMUAccessType access_type)
{
    /* Do not rearrange the CPUTLBEntry structure members. */
    QEMU_BUILD_BUG_ON(offsetof(CPUTLBEntry, addr_read) !=
                      MMU_DATA_LOAD * sizeof(uintptr_t));
    QEMU_BUILD_BUG_ON(offsetof(CPUTLBEntry, addr_write) !=
                      MMU_DATA_STORE * sizeof(uintptr_t));
    QEMU_BUILD_BUG_ON(offsetof(CPUTLBEntry, addr_code) !=
                      MMU_INST_FETCH * sizeof(uintptr_t));

    const uintptr_t *ptr = &entry->addr_idx[access_type];
    /* ofs might correspond to .addr_write, so use qatomic_read */
    return qatomic_read(ptr);
}

static inline uint64_t tlb_addr_write(const CPUTLBEntry *entry)
{
    return tlb_read_idx(entry, MMU_DATA_STORE);
}

/* Find the TLB index corresponding to the mmu_idx + address pair.  */
static inline uintptr_t tlb_index(CPUState *cpu, uintptr_t mmu_idx,
                                  vaddr addr)
{
    uintptr_t size_mask = cpu_tlb_fast(cpu, mmu_idx)->mask >> CPU_TLB_ENTRY_BITS;

    return (addr >> TARGET_PAGE_BITS) & size_mask;
}

/* Find the TLB entry corresponding to the mmu_idx + address pair.  */
static inline CPUTLBEntry *tlb_entry(CPUState *cpu, uintptr_t mmu_idx,
                                     vaddr addr)
{
    return &cpu_tlb_fast(cpu, mmu_idx)->table[tlb_index(cpu, mmu_idx, addr)];
}

static void tlb_window_reset(CPUTLBDesc *desc, int64_t ns,
                             size_t max_entries)
{
    desc->window_begin_ns = ns;
    desc->window_max_entries = max_entries;
}

void *it_acct_mmio_mr;

static void tb_jmp_cache_clear_page(CPUState *cpu, vaddr page_addr)
{
    CPUJumpCache *jc = cpu->tb_jmp_cache;
    CPUJumpCacheBank *bank;
    uint64_t epoch;
    int i, i0;

    if (unlikely(!jc)) {
        return;
    }

    i0 = tb_jmp_cache_hash_page(page_addr);
    TB_CACHE_CLEAR_NOTE(jc, TB_CLEAR_page_total);
    /* The page may be mapped in any address space; stale caches are skipped. */
    bank = jc->bank;
    epoch = qatomic_read(&bank->epoch);
    for (int c = 0; c < TB_JMP_CACHE_CONTEXTS; c++) {
        CPUJumpCache *each = &bank->cache[c];
        if (each != jc && each->epoch != epoch) {
            continue;
        }
        for (i = 0; i < TB_JMP_PAGE_SIZE; i++) {
            qatomic_set(&each->array[i0 + i].tb, NULL);
        }
    }
}

/**
 * tlb_mmu_resize_locked() - perform TLB resize bookkeeping; resize if necessary
 * @desc: The CPUTLBDesc portion of the TLB
 * @fast: The CPUTLBDescFast portion of the same TLB
 *
 * Called with tlb_lock_held.
 *
 * We have two main constraints when resizing a TLB: (1) we only resize it
 * on a TLB flush (otherwise we'd have to take a perf hit by either rehashing
 * the array or unnecessarily flushing it), which means we do not control how
 * frequently the resizing can occur; (2) we don't have access to the guest's
 * future scheduling decisions, and therefore have to decide the magnitude of
 * the resize based on past observations.
 *
 * In general, a memory-hungry process can benefit greatly from an appropriately
 * sized TLB, since a guest TLB miss is very expensive. This doesn't mean that
 * we just have to make the TLB as large as possible; while an oversized TLB
 * results in minimal TLB miss rates, it also takes longer to be flushed
 * (flushes can be _very_ frequent), and the reduced locality can also hurt
 * performance.
 *
 * To achieve near-optimal performance for all kinds of workloads, we:
 *
 * 1. Aggressively increase the size of the TLB when the use rate of the
 * TLB being flushed is high, since it is likely that in the near future this
 * memory-hungry process will execute again, and its memory hungriness will
 * probably be similar.
 *
 * 2. Slowly reduce the size of the TLB as the use rate declines over a
 * reasonably large time window. The rationale is that if in such a time window
 * we have not observed a high TLB use rate, it is likely that we won't observe
 * it in the near future. In that case, once a time window expires we downsize
 * the TLB to match the maximum use rate observed in the window.
 *
 * 3. Try to keep the maximum use rate in a time window in the 30-70% range,
 * since in that range performance is likely near-optimal. Recall that the TLB
 * is direct mapped, so we want the use rate to be low (or at least not too
 * high), since otherwise we are likely to have a significant amount of
 * conflict misses.
 */
static void tlb_mmu_resize_locked(CPUTLBDesc *desc, CPUTLBDescFast *fast,
                                  int64_t now)
{
    size_t old_size = tlb_n_entries(fast);
    size_t rate;
    size_t new_size = old_size;
    int64_t window_len_ms = 100;
    int64_t window_len_ns = window_len_ms * 1000 * 1000;
    bool window_expired = now > desc->window_begin_ns + window_len_ns;

    if (desc->n_used_entries > desc->window_max_entries) {
        desc->window_max_entries = desc->n_used_entries;
    }
    rate = desc->window_max_entries * 100 / old_size;

    if (rate > 70) {
        new_size = MIN(old_size << 1, 1 << CPU_TLB_DYN_MAX_BITS);
    } else if (rate < 30 && window_expired) {
        size_t ceil = pow2ceil(desc->window_max_entries);
        size_t expected_rate = desc->window_max_entries * 100 / ceil;

        /*
         * Avoid undersizing when the max number of entries seen is just below
         * a pow2. For instance, if max_entries == 1025, the expected use rate
         * would be 1025/2048==50%. However, if max_entries == 1023, we'd get
         * 1023/1024==99.9% use rate, so we'd likely end up doubling the size
         * later. Thus, make sure that the expected use rate remains below 70%.
         * (and since we double the size, that means the lowest rate we'd
         * expect to get is 35%, which is still in the 30-70% range where
         * we consider that the size is appropriate.)
         */
        if (expected_rate > 70) {
            ceil *= 2;
        }
        new_size = MAX(ceil, 1 << CPU_TLB_DYN_MIN_BITS);
    }

    if (new_size == old_size) {
        if (window_expired) {
            tlb_window_reset(desc, now, desc->n_used_entries);
        }
        return;
    }

    g_free(fast->table);
    g_free(desc->fulltlb);

    tlb_window_reset(desc, now, 0);
    /* desc->n_used_entries is cleared by the caller */
    fast->mask = (new_size - 1) << CPU_TLB_ENTRY_BITS;
    fast->table = g_try_new(CPUTLBEntry, new_size);
    desc->fulltlb = g_try_new(CPUTLBEntryFull, new_size);

    /*
     * If the allocations fail, try smaller sizes. We just freed some
     * memory, so going back to half of new_size has a good chance of working.
     * Increased memory pressure elsewhere in the system might cause the
     * allocations to fail though, so we progressively reduce the allocation
     * size, aborting if we cannot even allocate the smallest TLB we support.
     */
    while (fast->table == NULL || desc->fulltlb == NULL) {
        if (new_size == (1 << CPU_TLB_DYN_MIN_BITS)) {
            error_report("%s: %s", __func__, strerror(errno));
            abort();
        }
        new_size = MAX(new_size >> 1, 1 << CPU_TLB_DYN_MIN_BITS);
        fast->mask = (new_size - 1) << CPU_TLB_ENTRY_BITS;

        g_free(fast->table);
        g_free(desc->fulltlb);
        fast->table = g_try_new(CPUTLBEntry, new_size);
        desc->fulltlb = g_try_new(CPUTLBEntryFull, new_size);
    }
}

static void tlb_mmu_flush_locked(CPUTLBDesc *desc, CPUTLBDescFast *fast)
{
    desc->n_used_entries = 0;
    desc->large_page_addr = -1;
    desc->large_page_mask = -1;
    desc->vindex = 0;
    memset(fast->table, -1, sizeof_tlb(fast));
    memset(desc->vtable, -1, sizeof(desc->vtable));
}

static void tlb_flush_one_mmuidx_locked(CPUState *cpu, int mmu_idx,
                                        int64_t now)
{
    CPUTLBDesc *desc = &cpu->neg.tlb.d[mmu_idx];
    CPUTLBDescFast *fast = cpu_tlb_fast(cpu, mmu_idx);

    tlb_mmu_resize_locked(desc, fast, now);
    tlb_mmu_flush_locked(desc, fast);
}

static void tlb_mmu_init(CPUTLBDesc *desc, CPUTLBDescFast *fast, int64_t now)
{
    size_t n_entries = 1 << CPU_TLB_DYN_DEFAULT_BITS;

    tlb_window_reset(desc, now, 0);
    desc->n_used_entries = 0;
    fast->mask = (n_entries - 1) << CPU_TLB_ENTRY_BITS;
    fast->table = g_new(CPUTLBEntry, n_entries);
    desc->fulltlb = g_new(CPUTLBEntryFull, n_entries);
    tlb_mmu_flush_locked(desc, fast);
}

/*
 * Parked tables for one mmu index, by address space. A guest whose TLB is tagged (ARM's ASID) keeps a process's
 * entries across switches to another; QEMU's single table per index had to be flushed and refilled instead, ~27
 * refills per process switch on iOS 6. tlb_bank_switch parks the live table under the outgoing key and brings back
 * the incoming key's, if still held. Every flush that names the index also applies to the parked tables: a page
 * flush to each, anything wider drops them all, and tlb_flush_bank_key drops one key's (ARM TLBIASID). Only the
 * owning vCPU switches; everything is under tlb_c.lock.
 */
#define TLB_BANKS 8

#define TLB_BANK_RESETS 4

typedef struct TLBBank {
    CPUTLBDesc d;
    CPUTLBDescFast f;
    uint64_t key;
    uint64_t used;          /* LRU stamp; 0 = holds nothing usable */
    /* tlb_reset_dirty ranges (host addresses) not applied yet: done on the way back in */
    uintptr_t reset_start[TLB_BANK_RESETS], reset_len[TLB_BANK_RESETS];
    unsigned nreset;
} TLBBank;

struct TLBBankSet {
    int mmu_idx;
    uint64_t clock;
    TLBBank b[TLB_BANKS];
};

static void tlb_bank_swap(CPUState *cpu, int mmu_idx, TLBBank *b)
{
    CPUTLBDesc d = cpu->neg.tlb.d[mmu_idx];
    CPUTLBDescFast f = *cpu_tlb_fast(cpu, mmu_idx);

    cpu->neg.tlb.d[mmu_idx] = b->d;
    *cpu_tlb_fast(cpu, mmu_idx) = b->f;
    b->d = d;
    b->f = f;
}

static inline bool tlb_banks_on(CPUState *cpu, MMUIdxMap idxmap)
{
    struct TLBBankSet *set = cpu->neg.tlb.c.banks;

    return unlikely(set) && ((idxmap >> set->mmu_idx) & 1);
}

static void tlb_banks_drop_locked(CPUState *cpu)
{
    struct TLBBankSet *set = cpu->neg.tlb.c.banks;

    for (int i = 0; i < TLB_BANKS; i++) {
        set->b[i].used = 0;
    }
}

static bool tlb_flush_entry_locked(CPUTLBEntry *tlb_entry, vaddr page);

static void tlb_banks_flush_page_locked(CPUState *cpu, vaddr page)
{
    struct TLBBankSet *set = cpu->neg.tlb.c.banks;

    for (int i = 0; i < TLB_BANKS; i++) {
        TLBBank *b = &set->b[i];

        if (!b->used) {
            continue;
        }
        if ((page & b->d.large_page_mask) == b->d.large_page_addr) {
            b->used = 0;
            continue;
        }
        if (tlb_flush_entry_locked(&b->f.table[(page >> TARGET_PAGE_BITS) & (b->f.mask >> CPU_TLB_ENTRY_BITS)],
                                   page)) {
            b->d.n_used_entries--;
        }
        for (int k = 0; k < CPU_VTLB_SIZE; k++) {
            if (tlb_flush_entry_locked(&b->d.vtable[k], page)) {
                b->d.n_used_entries--;
            }
        }
    }
}

static inline int64_t tlb_clock_ns(void);
static void tlb_reset_dirty_range_locked(CPUTLBEntryFull *full, CPUTLBEntry *ent,
                                         uintptr_t start, uintptr_t length);

/* Apply the dirty-tracking resets the live table missed while it was parked. */
static void tlb_bank_catch_up_locked(CPUState *cpu, int mmu_idx, TLBBank *b)
{
    CPUTLBDesc *desc = &cpu->neg.tlb.d[mmu_idx];
    CPUTLBDescFast *fast = cpu_tlb_fast(cpu, mmu_idx);
    unsigned int n = tlb_n_entries(fast);

    for (unsigned r = 0; r < b->nreset; r++) {
        for (unsigned int i = 0; i < n; i++) {
            tlb_reset_dirty_range_locked(&desc->fulltlb[i], &fast->table[i], b->reset_start[r], b->reset_len[r]);
        }
        for (unsigned int i = 0; i < CPU_VTLB_SIZE; i++) {
            tlb_reset_dirty_range_locked(&desc->vfulltlb[i], &desc->vtable[i], b->reset_start[r], b->reset_len[r]);
        }
    }
    b->nreset = 0;
}

void tlb_bank_switch(CPUState *cpu, int mmu_idx, uint64_t old_key, uint64_t new_key)
{
    struct TLBBankSet *set;
    TLBBank *in = NULL, *out = NULL;
    bool live;

    assert_cpu_is_self(cpu);
    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    set = cpu->neg.tlb.c.banks;
    if (!set) {
        int64_t now = tlb_clock_ns();

        set = g_new0(struct TLBBankSet, 1);
        set->mmu_idx = mmu_idx;
        for (int i = 0; i < TLB_BANKS; i++) {
            tlb_mmu_init(&set->b[i].d, &set->b[i].f, now);
        }
        cpu->neg.tlb.c.banks = set;
    }
    assert(set->mmu_idx == mmu_idx);
    if (old_key == new_key) {
        qemu_spin_unlock(&cpu->neg.tlb.c.lock);
        return;
    }
    for (int i = 0; i < TLB_BANKS; i++) {
        TLBBank *b = &set->b[i];

        if (b->used && b->key == new_key) {
            in = b;
        } else if (b->used && b->key == old_key) {
            out = b;
        }
    }
    if (!out) {
        for (int i = 0; i < TLB_BANKS; i++) {
            TLBBank *b = &set->b[i];

            if (b != in && (!out || b->used < out->used)) {
                out = b;
            }
        }
    }
    /* Park the live table; what comes back from that slot is stale until flushed or swapped out again. */
    live = cpu->neg.tlb.c.dirty & (1 << mmu_idx);
    tlb_bank_swap(cpu, mmu_idx, out);
    out->key = old_key;
    out->used = live ? ++set->clock : 0;
    out->nreset = 0;
    if (in) {
        CPUTLBDescFast *fast = cpu_tlb_fast(cpu, mmu_idx);
        size_t size;

        tlb_bank_swap(cpu, mmu_idx, in);
        tlb_bank_catch_up_locked(cpu, mmu_idx, in);
        in->used = 0;
        cpu->neg.tlb.c.dirty |= 1 << mmu_idx;
        /* A table that is never flushed is never resized either: grow (or shrink) it here, at the price of its entries. */
        size = tlb_n_entries(fast);
        tlb_mmu_resize_locked(&cpu->neg.tlb.d[mmu_idx], fast, tlb_clock_ns());
        if (tlb_n_entries(fast) != size) {
            tlb_mmu_flush_locked(&cpu->neg.tlb.d[mmu_idx], fast);
            cpu->neg.tlb.c.dirty &= ~(1 << mmu_idx);
        }
    } else {
        tlb_flush_one_mmuidx_locked(cpu, mmu_idx, tlb_clock_ns());
        cpu->neg.tlb.c.dirty &= ~(1 << mmu_idx);
    }
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);
}

void tlb_flush_bank_key(CPUState *cpu, uint64_t key)
{
    struct TLBBankSet *set;

    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    set = cpu->neg.tlb.c.banks;
    for (int i = 0; set && i < TLB_BANKS; i++) {
        if (set->b[i].used && set->b[i].key == key) {
            set->b[i].used = 0;
        }
    }
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);
}

/*
 * The clock for TLB resizing, which only compares against a 100 ms window. Every flush reads it, and iOS flushes on
 * each context switch (and fully whenever a page flush hits a section): gettimeofday is ~200 ns on a phone (the
 * counter read), the coarse clock ~12 ns.
 */
static inline int64_t tlb_clock_ns(void)
{
#ifdef CLOCK_MONOTONIC_COARSE
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
#else
    return get_clock_realtime();
#endif
}

static inline void tlb_n_used_entries_inc(CPUState *cpu, uintptr_t mmu_idx)
{
    cpu->neg.tlb.d[mmu_idx].n_used_entries++;
}

static inline void tlb_n_used_entries_dec(CPUState *cpu, uintptr_t mmu_idx)
{
    cpu->neg.tlb.d[mmu_idx].n_used_entries--;
}

/* A CPU's queue of broadcast TLB flushes (see tlb_shootdown_inline_run). */
#define TLB_SD_OPS 32

typedef struct TLBShootdownOp {
    vaddr addr, len;
    MMUIdxMap idxmap;
    uint8_t kind, bits;
} TLBShootdownOp;

struct TLBShootdownQueue {
    QemuSpin lock;
    unsigned n;
    bool all;
    uint64_t enq, done;
    TLBShootdownOp op[TLB_SD_OPS];
};

static int tlb_shootdown_mode(void);

void tlb_init(CPUState *cpu)
{
    int64_t now = tlb_clock_ns();
    int i;

    qemu_spin_init(&cpu->neg.tlb.c.lock);
    if (!cpu->tlb_sd) {
        cpu->tlb_sd = g_new0(struct TLBShootdownQueue, 1);
        qemu_spin_init(&cpu->tlb_sd->lock);
    }
    tlb_shootdown_mode();

    /* All tlbs are initialized flushed. */
    cpu->neg.tlb.c.dirty = 0;

    for (i = 0; i < NB_MMU_MODES; i++) {
        tlb_mmu_init(&cpu->neg.tlb.d[i], cpu_tlb_fast(cpu, i), now);
    }
}

void tlb_destroy(CPUState *cpu)
{
    int i;

    qemu_spin_destroy(&cpu->neg.tlb.c.lock);
    for (i = 0; i < NB_MMU_MODES; i++) {
        CPUTLBDesc *desc = &cpu->neg.tlb.d[i];
        CPUTLBDescFast *fast = cpu_tlb_fast(cpu, i);

        g_free(fast->table);
        g_free(desc->fulltlb);
    }
    for (i = 0; cpu->neg.tlb.c.banks && i < TLB_BANKS; i++) {
        g_free(cpu->neg.tlb.c.banks->b[i].f.table);
        g_free(cpu->neg.tlb.c.banks->b[i].d.fulltlb);
    }
    g_clear_pointer(&cpu->neg.tlb.c.banks, g_free);
}

/* flush_all_helper: run fn across all cpus
 *
 * If the wait flag is set then the src cpu's helper will be queued as
 * "safe" work and the loop exited creating a synchronisation point
 * where all queued work will be finished before execution starts
 * again.
 */
/*
 * Whether a broadcast TLB flush from @src reaches @cpu: only within @src's TLB domain (CPUState tlb_domain).
 * LT_TLB_DOMAIN=off reaches every CPU again (A/B). Waking the A6's IOP and AE2 cores for each of the AP's ~1.3K
 * broadcasts a second made each take the BQL as often, for translations they never hold.
 */
static bool tlb_flush_reaches(CPUState *src, CPUState *cpu)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("LT_TLB_DOMAIN");
        on = !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
    }
    return cpu != src && (!on || cpu->tlb_domain == src->tlb_domain);
}

static void flush_all_helper(CPUState *src, run_on_cpu_func fn,
                             run_on_cpu_data d)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (tlb_flush_reaches(src, cpu)) {
            async_run_on_cpu(cpu, fn, d);
        }
    }
}

/*
 * A guest's broadcast TLB maintenance (TLBI...IS) must be complete on every CPU it reaches before the CPU that issued
 * it runs on. QEMU did that with exclusive work: the issuer stopped every vCPU in the machine, flushed, and let them
 * go. iOS 10 on the A6 issues ~0.6-1.4K page broadcasts a second, and the stops cost 3-9% of wall time with every vCPU
 * stopped. Instead the issuer flushes its own TLB at once, queues the flush on each CPU the broadcast reaches, and
 * before running guest code again waits until each of those has run it or is outside guest code (it then runs its
 * queued work before it executes again: the queueing kicked it, so it leaves cpu_exec at its next TB boundary).
 * Nothing else stops. LT_TLB_SHOOTDOWN=off brings back the exclusive work (A/B).
 */
#define TLB_SHOOTDOWN_MAX 16

enum { TLB_SD_PAGE, TLB_SD_RANGE, TLB_SD_ALL };
static bool tlb_shootdown_inline_run(CPUState *src, int kind, vaddr addr, vaddr len, MMUIdxMap idxmap,
                                     unsigned bits);

typedef struct TLBShootdown {
    int refs;
    int n;
    CPUState *targets[TLB_SHOOTDOWN_MAX];
    bool acked[TLB_SHOOTDOWN_MAX];
} TLBShootdown;

typedef struct TLBShootdownItem {
    run_on_cpu_func fn;
    run_on_cpu_data d;
    TLBShootdown *sd;
    int slot;
} TLBShootdownItem;

/*
 * LT_TLB_SHOOTDOWN: unset or "on", the targets flush at their next TB boundary and the issuer waits in place
 * (tlb_shootdown_inline_run); "queued", they run the flush as queued work (tlb_broadcast_synced); "off", the
 * exclusive work. Only from @src's own thread, which flushes itself directly; from elsewhere, the exclusive work.
 */
bool tlb_shootdown_inline;

static int tlb_shootdown_mode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *v = getenv("LT_TLB_SHOOTDOWN");
        mode = v && (!strcmp(v, "off") || !strcmp(v, "0")) ? 0 : v && !strcmp(v, "queued") ? 1 : 2;
        tlb_shootdown_inline = mode == 2;
    }
    return mode;
}

static bool tlb_shootdown_on(CPUState *src)
{
    return tlb_shootdown_mode() && current_cpu == src;
}

static void tlb_shootdown_unref(TLBShootdown *sd)
{
    if (qatomic_fetch_dec(&sd->refs) == 1) {
        g_free(sd);
    }
}

static void tlb_shootdown_flush(CPUState *cpu, run_on_cpu_data data)
{
    TLBShootdownItem *it = data.host_ptr;

    it->fn(cpu, it->d);
    qatomic_store_release(&it->sd->acked[it->slot], true);
    tlb_shootdown_unref(it->sd);
    g_free(it);
}

/* Queued on the issuer itself: runs once it has left cpu_exec, with the BQL held, which the targets may need. */
static void tlb_shootdown_wait(CPUState *src, run_on_cpu_data data)
{
    TLBShootdown *sd = data.host_ptr;
    int64_t t0 = it_acct_excl_on ? get_clock() : 0;

    bql_unlock();
    smp_mb();
    for (int i = 0; i < sd->n; i++) {
        CPUState *t = sd->targets[i];
        unsigned spins = 0;

        while (!qatomic_load_acquire(&sd->acked[i]) && qatomic_read(&t->running)) {
            if (++spins < 2000) {
                cpu_relax();
            } else {
                g_usleep(0);
            }
            smp_mb();
        }
    }
    if (t0) {
        /* In the exclusive table, as the work it replaces: the issuer's wait, no stop of the others. */
        it_acct_excl_note((void *)tlb_shootdown_wait, get_clock() - t0, 0);
    }
    bql_lock();
    tlb_shootdown_unref(sd);
}

/*
 * fn(cpu, d) on @src now and on every CPU the broadcast reaches, synced as above. @size > 0: d.host_ptr is a block
 * of that size that fn frees, copied for each target; otherwise d is passed as it is.
 */
static void tlb_broadcast_synced(CPUState *src, run_on_cpu_func fn, run_on_cpu_data d, size_t size)
{
    TLBShootdown *sd = g_new0(TLBShootdown, 1);
    CPUState *cpu;

    sd->refs = 1;
    CPU_FOREACH(cpu) {
        if (tlb_flush_reaches(src, cpu)) {
            TLBShootdownItem *it = g_new(TLBShootdownItem, 1);

            g_assert(sd->n < TLB_SHOOTDOWN_MAX);
            it->fn = fn;
            it->d = size ? RUN_ON_CPU_HOST_PTR(g_memdup2(d.host_ptr, size)) : d;
            it->sd = sd;
            it->slot = sd->n;
            sd->targets[sd->n++] = cpu;
            qatomic_inc(&sd->refs);
            async_run_on_cpu(cpu, tlb_shootdown_flush, RUN_ON_CPU_HOST_PTR(it));
        }
    }
    fn(src, d);
    if (sd->n) {
        async_run_on_cpu(src, tlb_shootdown_wait, RUN_ON_CPU_HOST_PTR(sd));
    } else {
        tlb_shootdown_unref(sd);
    }
}

/* In a flush's work data: the guest switched address space, see tlb_flush_switch_context. */
#define TLB_FLUSH_KEEP_JMP_CACHE (1u << 31)
QEMU_BUILD_BUG_ON(NB_MMU_MODES > 31);

static void tlb_flush_by_mmuidx_async_work(CPUState *cpu, run_on_cpu_data data)
{
    MMUIdxMap asked = data.host_int & ~TLB_FLUSH_KEEP_JMP_CACHE;
    bool keep_jmp_cache = data.host_int & TLB_FLUSH_KEEP_JMP_CACHE;
    MMUIdxMap all_dirty, work, to_clean;
    int64_t now = tlb_clock_ns();

    assert_cpu_is_self(cpu);

    tlb_debug("mmu_idx:0x%04" PRIx16 "\n", asked);

    qemu_spin_lock(&cpu->neg.tlb.c.lock);

    if (tlb_banks_on(cpu, asked)) {
        tlb_banks_drop_locked(cpu);
    }
    all_dirty = cpu->neg.tlb.c.dirty;
    to_clean = asked & all_dirty;
    all_dirty &= ~to_clean;
    cpu->neg.tlb.c.dirty = all_dirty;

    for (work = to_clean; work != 0; work &= work - 1) {
        int mmu_idx = ctz32(work);
        tlb_flush_one_mmuidx_locked(cpu, mmu_idx, now);
    }

    qemu_spin_unlock(&cpu->neg.tlb.c.lock);

    TB_CACHE_CLEAR_NOTE(cpu->tb_jmp_cache, TB_CLEAR_full_tlb);
    TB_CACHE_CLEAR_NOTE(cpu->tb_jmp_cache,
                       !to_clean ? TB_CLEAR_tlb_clean :
                       to_clean == ALL_MMUIDX_BITS ? TB_CLEAR_tlb_all :
                       TB_CLEAR_tlb_partial);
    if (!keep_jmp_cache) {
        tcg_flush_jmp_cache(cpu);
    }

    if (to_clean == ALL_MMUIDX_BITS) {
        qatomic_set(&cpu->neg.tlb.c.full_flush_count,
                    cpu->neg.tlb.c.full_flush_count + 1);
    } else {
        qatomic_set(&cpu->neg.tlb.c.part_flush_count,
                    cpu->neg.tlb.c.part_flush_count + ctpop16(to_clean));
        if (to_clean != asked) {
            qatomic_set(&cpu->neg.tlb.c.elide_flush_count,
                        cpu->neg.tlb.c.elide_flush_count +
                        ctpop16(asked & ~to_clean));
        }
    }
}

void tlb_flush_by_mmuidx(CPUState *cpu, MMUIdxMap idxmap)
{
    tlb_debug("mmu_idx: 0x%" PRIx16 "\n", idxmap);

    assert_cpu_is_self(cpu);

    tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT(idxmap));
}

void tlb_flush(CPUState *cpu)
{
    tlb_flush_by_mmuidx(cpu, ALL_MMUIDX_BITS);
}

extern bool it_acct_on;
uint64_t it_acct_ctxsw, it_acct_fills[32][2];
uint64_t it_acct_el0_sync, it_acct_el0_used, it_acct_tlbiasid, it_acct_pflush, it_acct_rflush, it_acct_reset_dirty;

void tlb_flush_switch_context(CPUState *cpu, uint64_t key)
{
    assert_cpu_is_self(cpu);
    if (unlikely(it_acct_on)) {
        it_acct_ctxsw++;
    }
    tcg_switch_jmp_cache(cpu, key);
    tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT(ALL_MMUIDX_BITS |
                                                        TLB_FLUSH_KEEP_JMP_CACHE));
}

void tlb_flush_context(CPUState *cpu, uint64_t key)
{
    assert_cpu_is_self(cpu);
    switch (tcg_flush_jmp_cache_key(cpu, key)) {
    case 1:
        tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT(ALL_MMUIDX_BITS |
                                                            TLB_FLUSH_KEEP_JMP_CACHE));
        break;
    case 0:
        /* another address space: nothing of it is in the TLB, but it may be parked */
        tlb_flush_bank_key(cpu, key);
        break;
    default:
        tlb_flush(cpu);
        break;
    }
}

void tlb_flush_by_mmuidx_all_cpus_synced(CPUState *src_cpu, MMUIdxMap idxmap)
{
    const run_on_cpu_func fn = tlb_flush_by_mmuidx_async_work;

    tlb_debug("mmu_idx: 0x%"PRIx16"\n", idxmap);

    if (tlb_shootdown_inline_run(src_cpu, TLB_SD_ALL, 0, 0, idxmap, 0)) {
        return;
    }
    if (tlb_shootdown_on(src_cpu)) {
        tlb_broadcast_synced(src_cpu, fn, RUN_ON_CPU_HOST_INT(idxmap), 0);
        return;
    }
    flush_all_helper(src_cpu, fn, RUN_ON_CPU_HOST_INT(idxmap));
    async_safe_run_on_cpu(src_cpu, fn, RUN_ON_CPU_HOST_INT(idxmap));
}

void tlb_flush_all_cpus_synced(CPUState *src_cpu)
{
    tlb_flush_by_mmuidx_all_cpus_synced(src_cpu, ALL_MMUIDX_BITS);
}

static bool tlb_hit_page_mask_anyprot(CPUTLBEntry *tlb_entry,
                                      vaddr page, vaddr mask)
{
    page &= mask;
    mask &= TARGET_PAGE_MASK | TLB_INVALID_MASK;

    return (page == (tlb_entry->addr_read & mask) ||
            page == (tlb_addr_write(tlb_entry) & mask) ||
            page == (tlb_entry->addr_code & mask));
}

static inline bool tlb_hit_page_anyprot(CPUTLBEntry *tlb_entry, vaddr page)
{
    return tlb_hit_page_mask_anyprot(tlb_entry, page, -1);
}

/**
 * tlb_entry_is_empty - return true if the entry is not in use
 * @te: pointer to CPUTLBEntry
 */
static inline bool tlb_entry_is_empty(const CPUTLBEntry *te)
{
    return te->addr_read == -1 && te->addr_write == -1 && te->addr_code == -1;
}

/* Called with tlb_c.lock held */
static bool tlb_flush_entry_mask_locked(CPUTLBEntry *tlb_entry,
                                        vaddr page,
                                        vaddr mask)
{
    if (tlb_hit_page_mask_anyprot(tlb_entry, page, mask)) {
        memset(tlb_entry, -1, sizeof(*tlb_entry));
        return true;
    }
    return false;
}

static inline bool tlb_flush_entry_locked(CPUTLBEntry *tlb_entry, vaddr page)
{
    return tlb_flush_entry_mask_locked(tlb_entry, page, -1);
}

/* Called with tlb_c.lock held */
static void tlb_flush_vtlb_page_mask_locked(CPUState *cpu, int mmu_idx,
                                            vaddr page,
                                            vaddr mask)
{
    CPUTLBDesc *d = &cpu->neg.tlb.d[mmu_idx];
    int k;

    assert_cpu_is_self(cpu);
    for (k = 0; k < CPU_VTLB_SIZE; k++) {
        if (tlb_flush_entry_mask_locked(&d->vtable[k], page, mask)) {
            tlb_n_used_entries_dec(cpu, mmu_idx);
        }
    }
}

static inline void tlb_flush_vtlb_page_locked(CPUState *cpu, int mmu_idx,
                                              vaddr page)
{
    tlb_flush_vtlb_page_mask_locked(cpu, mmu_idx, page, -1);
}

/*
 * The indexes of idxmap a page or range flush has to visit: only the dirty ones, as an index is marked dirty by
 * every fill and clean again only by a full flush, so a clean one holds no entries, victim TLB included. An index
 * nothing fills shrinks to the minimum size, where flushing one 4 KiB page means flushing the whole table: on iOS 6,
 * whose kernel invalidates ~10K pages a second, that was ~15K full flushes a second of empty tables.
 * LT_TLB_FLUSH_DIRTY_ONLY=off visits them all (A/B).
 */
static MMUIdxMap tlb_flush_clean_skip(CPUState *cpu, MMUIdxMap idxmap)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("LT_TLB_FLUSH_DIRTY_ONLY");
        on = !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
    }
    return on ? idxmap & cpu->neg.tlb.c.dirty : idxmap;
}

static void tlb_flush_page_locked(CPUState *cpu, int midx, vaddr page)
{
    vaddr lp_addr = cpu->neg.tlb.d[midx].large_page_addr;
    vaddr lp_mask = cpu->neg.tlb.d[midx].large_page_mask;


    /* Check if we need to flush due to large pages.  */
    if ((page & lp_mask) == lp_addr) {
        tlb_debug("forcing full flush midx %d (%016"
                  VADDR_PRIx "/%016" VADDR_PRIx ")\n",
                  midx, lp_addr, lp_mask);
        tlb_flush_one_mmuidx_locked(cpu, midx, tlb_clock_ns());
    } else {
        if (tlb_flush_entry_locked(tlb_entry(cpu, midx, page), page)) {
            tlb_n_used_entries_dec(cpu, midx);
        }
        tlb_flush_vtlb_page_locked(cpu, midx, page);
    }
}

/**
 * tlb_flush_page_by_mmuidx_async_0:
 * @cpu: cpu on which to flush
 * @addr: page of virtual address to flush
 * @idxmap: set of mmu_idx to flush
 *
 * Helper for tlb_flush_page_by_mmuidx and friends, flush one page
 * at @addr from the tlbs indicated by @idxmap from @cpu.
 */
static void tlb_flush_page_by_mmuidx_async_0(CPUState *cpu,
                                             vaddr addr,
                                             MMUIdxMap idxmap)
{
    int mmu_idx;

    assert_cpu_is_self(cpu);

    tlb_debug("page addr: %016" VADDR_PRIx " mmu_map:0x%x\n", addr, idxmap);

    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    /* A clean index holds no entries (see tlb_flush_clean_skip). */
    if (unlikely(it_acct_on)) {
        it_acct_pflush++;
    }
    if (tlb_banks_on(cpu, idxmap)) {
        tlb_banks_flush_page_locked(cpu, addr);
    }
    idxmap = tlb_flush_clean_skip(cpu, idxmap);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        if ((idxmap >> mmu_idx) & 1) {
            tlb_flush_page_locked(cpu, mmu_idx, addr);
        }
    }
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);

    /*
     * Discard jump cache entries for any tb which might potentially
     * overlap the flushed page, which includes the previous.
     */
    TB_CACHE_CLEAR_NOTE(cpu->tb_jmp_cache, TB_CLEAR_page_previous);
    tb_jmp_cache_clear_page(cpu, addr - TARGET_PAGE_SIZE);
    TB_CACHE_CLEAR_NOTE(cpu->tb_jmp_cache, TB_CLEAR_page_current);
    tb_jmp_cache_clear_page(cpu, addr);
}

void tlb_flush_by_mmuidx_keep_jmp_cache(CPUState *cpu, MMUIdxMap idxmap)
{
    assert_cpu_is_self(cpu);
    tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT(idxmap | TLB_FLUSH_KEEP_JMP_CACHE));
}

void tlb_flush_switch_context_keep(CPUState *cpu, uint64_t key, int keep_idx,
                                   const vaddr *pages, unsigned n, MMUIdxMap defer)
{
    if (keep_idx < 0) {
        tlb_flush_switch_context(cpu, key);
        return;
    }
    assert_cpu_is_self(cpu);
    if (unlikely(it_acct_on)) {
        it_acct_ctxsw++;
    }
    tcg_switch_jmp_cache(cpu, key);
    tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT((ALL_MMUIDX_BITS & ~(1 << keep_idx) &
                                                         ~defer) | TLB_FLUSH_KEEP_JMP_CACHE));
    /* TLB entries only: the jump cache is already the new context's. */
    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    for (unsigned i = 0; i < n; i++) {
        tlb_flush_page_locked(cpu, keep_idx, pages[i]);
    }
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);
}

/**
 * tlb_flush_page_by_mmuidx_async_1:
 * @cpu: cpu on which to flush
 * @data: encoded addr + idxmap
 *
 * Helper for tlb_flush_page_by_mmuidx and friends, called through
 * async_run_on_cpu.  The idxmap parameter is encoded in the page
 * offset of the target_ptr field.  This limits the set of mmu_idx
 * that can be passed via this method.
 */
static void tlb_flush_page_by_mmuidx_async_1(CPUState *cpu,
                                             run_on_cpu_data data)
{
    vaddr addr_and_idxmap = data.target_ptr;
    vaddr addr = addr_and_idxmap & TARGET_PAGE_MASK;
    MMUIdxMap idxmap = addr_and_idxmap & ~TARGET_PAGE_MASK;

    tlb_flush_page_by_mmuidx_async_0(cpu, addr, idxmap);
}

typedef struct {
    vaddr addr;
    MMUIdxMap idxmap;
} TLBFlushPageByMMUIdxData;

/**
 * tlb_flush_page_by_mmuidx_async_2:
 * @cpu: cpu on which to flush
 * @data: allocated addr + idxmap
 *
 * Helper for tlb_flush_page_by_mmuidx and friends, called through
 * async_run_on_cpu.  The addr+idxmap parameters are stored in a
 * TLBFlushPageByMMUIdxData structure that has been allocated
 * specifically for this helper.  Free the structure when done.
 */
static void tlb_flush_page_by_mmuidx_async_2(CPUState *cpu,
                                             run_on_cpu_data data)
{
    TLBFlushPageByMMUIdxData *d = data.host_ptr;

    tlb_flush_page_by_mmuidx_async_0(cpu, d->addr, d->idxmap);
    g_free(d);
}

void tlb_flush_page_by_mmuidx(CPUState *cpu, vaddr addr, MMUIdxMap idxmap)
{
    tlb_debug("addr: %016" VADDR_PRIx " mmu_idx:%" PRIx16 "\n", addr, idxmap);

    assert_cpu_is_self(cpu);

    /* This should already be page aligned */
    addr &= TARGET_PAGE_MASK;

    tlb_flush_page_by_mmuidx_async_0(cpu, addr, idxmap);
}

void tlb_flush_page(CPUState *cpu, vaddr addr)
{
    tlb_flush_page_by_mmuidx(cpu, addr, ALL_MMUIDX_BITS);
}

void tlb_flush_page_by_mmuidx_all_cpus_synced(CPUState *src_cpu,
                                              vaddr addr,
                                              MMUIdxMap idxmap)
{
    tlb_debug("addr: %016" VADDR_PRIx " mmu_idx:%"PRIx16"\n", addr, idxmap);

    /* This should already be page aligned */
    addr &= TARGET_PAGE_MASK;

    /*
     * Allocate memory to hold addr+idxmap only when needed.
     * See tlb_flush_page_by_mmuidx for details.
     */
    if (tlb_shootdown_inline_run(src_cpu, TLB_SD_PAGE, addr, 0, idxmap, 0)) {
        return;
    }
    if (tlb_shootdown_on(src_cpu)) {
        if (idxmap < TARGET_PAGE_SIZE) {
            tlb_broadcast_synced(src_cpu, tlb_flush_page_by_mmuidx_async_1,
                                 RUN_ON_CPU_TARGET_PTR(addr | idxmap), 0);
        } else {
            TLBFlushPageByMMUIdxData *d = g_new(TLBFlushPageByMMUIdxData, 1);

            d->addr = addr;
            d->idxmap = idxmap;
            tlb_broadcast_synced(src_cpu, tlb_flush_page_by_mmuidx_async_2, RUN_ON_CPU_HOST_PTR(d), sizeof(*d));
        }
        return;
    }
    if (idxmap < TARGET_PAGE_SIZE) {
        flush_all_helper(src_cpu, tlb_flush_page_by_mmuidx_async_1,
                         RUN_ON_CPU_TARGET_PTR(addr | idxmap));
        async_safe_run_on_cpu(src_cpu, tlb_flush_page_by_mmuidx_async_1,
                              RUN_ON_CPU_TARGET_PTR(addr | idxmap));
    } else {
        CPUState *dst_cpu;
        TLBFlushPageByMMUIdxData *d;

        /* Allocate a separate data block for each destination cpu.  */
        CPU_FOREACH(dst_cpu) {
            if (tlb_flush_reaches(src_cpu, dst_cpu)) {
                d = g_new(TLBFlushPageByMMUIdxData, 1);
                d->addr = addr;
                d->idxmap = idxmap;
                async_run_on_cpu(dst_cpu, tlb_flush_page_by_mmuidx_async_2,
                                 RUN_ON_CPU_HOST_PTR(d));
            }
        }

        d = g_new(TLBFlushPageByMMUIdxData, 1);
        d->addr = addr;
        d->idxmap = idxmap;
        async_safe_run_on_cpu(src_cpu, tlb_flush_page_by_mmuidx_async_2,
                              RUN_ON_CPU_HOST_PTR(d));
    }
}

void tlb_flush_page_all_cpus_synced(CPUState *src, vaddr addr)
{
    tlb_flush_page_by_mmuidx_all_cpus_synced(src, addr, ALL_MMUIDX_BITS);
}

static void tlb_flush_range_locked(CPUState *cpu, int midx,
                                   vaddr addr, vaddr len,
                                   unsigned bits)
{
    CPUTLBDesc *d = &cpu->neg.tlb.d[midx];
    CPUTLBDescFast *f = cpu_tlb_fast(cpu, midx);
    vaddr mask = MAKE_64BIT_MASK(0, bits);

    /*
     * If @bits is smaller than the tlb size, there may be multiple entries
     * within the TLB; otherwise all addresses that match under @mask hit
     * the same TLB entry.
     * TODO: Perhaps allow bits to be a few bits less than the size.
     * For now, just flush the entire TLB.
     *
     * If @len is larger than the tlb size, then it will take longer to
     * test all of the entries in the TLB than it will to flush it all.
     */
    if (mask < f->mask || len > f->mask) {
        tlb_debug("forcing full flush midx %d ("
                  "%016" VADDR_PRIx "/%016" VADDR_PRIx "+%016" VADDR_PRIx ")\n",
                  midx, addr, mask, len);
        tlb_flush_one_mmuidx_locked(cpu, midx, tlb_clock_ns());
        return;
    }

    /*
     * Check if we need to flush due to large pages.
     * Because large_page_mask contains all 1's from the msb,
     * we only need to test the end of the range.
     */
    if (((addr + len - 1) & d->large_page_mask) == d->large_page_addr) {
        tlb_debug("forcing full flush midx %d ("
                  "%016" VADDR_PRIx "/%016" VADDR_PRIx ")\n",
                  midx, d->large_page_addr, d->large_page_mask);
        tlb_flush_one_mmuidx_locked(cpu, midx, tlb_clock_ns());
        return;
    }

    for (vaddr i = 0; i < len; i += TARGET_PAGE_SIZE) {
        vaddr page = addr + i;
        CPUTLBEntry *entry = tlb_entry(cpu, midx, page);

        if (tlb_flush_entry_mask_locked(entry, page, mask)) {
            tlb_n_used_entries_dec(cpu, midx);
        }
        tlb_flush_vtlb_page_mask_locked(cpu, midx, page, mask);
    }
}

typedef struct {
    vaddr addr;
    vaddr len;
    MMUIdxMap idxmap;
    unsigned bits;
} TLBFlushRangeData;

static void tlb_flush_range_by_mmuidx_async_0(CPUState *cpu,
                                              TLBFlushRangeData d)
{
    int mmu_idx;

    assert_cpu_is_self(cpu);

    tlb_debug("range: %016" VADDR_PRIx "/%u+%016" VADDR_PRIx " mmu_map:0x%x\n",
              d.addr, d.bits, d.len, d.idxmap);

    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    if (unlikely(it_acct_on)) {
        it_acct_rflush++;
    }
    if (tlb_banks_on(cpu, d.idxmap)) {
        tlb_banks_drop_locked(cpu);
    }
    d.idxmap = tlb_flush_clean_skip(cpu, d.idxmap);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        if ((d.idxmap >> mmu_idx) & 1) {
            tlb_flush_range_locked(cpu, mmu_idx, d.addr, d.len, d.bits);
        }
    }
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);

    /*
     * If the length is larger than the jump cache size, then it will take
     * longer to clear each entry individually than it will to clear it all.
     */
    if (d.len >= (TARGET_PAGE_SIZE * TB_JMP_CACHE_SIZE)) {
        TB_CACHE_CLEAR_NOTE(cpu->tb_jmp_cache, TB_CLEAR_full_range);
        tcg_flush_jmp_cache(cpu);
        return;
    }

    /*
     * Discard jump cache entries for any tb which might potentially
     * overlap the flushed pages, which includes the previous.
     */
    d.addr -= TARGET_PAGE_SIZE;
    for (vaddr i = 0, n = d.len / TARGET_PAGE_SIZE + 1; i < n; i++) {
        TB_CACHE_CLEAR_NOTE(cpu->tb_jmp_cache,
                           i == 0 ? TB_CLEAR_page_previous :
                           TB_CLEAR_page_range);
        tb_jmp_cache_clear_page(cpu, d.addr);
        d.addr += TARGET_PAGE_SIZE;
    }
}

static void tlb_flush_range_by_mmuidx_async_1(CPUState *cpu,
                                              run_on_cpu_data data)
{
    TLBFlushRangeData *d = data.host_ptr;
    tlb_flush_range_by_mmuidx_async_0(cpu, *d);
    g_free(d);
}

void tlb_flush_range_by_mmuidx(CPUState *cpu, vaddr addr,
                               vaddr len, MMUIdxMap idxmap,
                               unsigned bits)
{
    TLBFlushRangeData d;

    assert_cpu_is_self(cpu);

    /* If no page bits are significant, this devolves to tlb_flush. */
    if (bits < TARGET_PAGE_BITS) {
        tlb_flush_by_mmuidx(cpu, idxmap);
        return;
    }
    /*
     * If all bits are significant, and len is small,
     * this devolves to tlb_flush_page.
     */
    if (len <= TARGET_PAGE_SIZE && bits >= target_long_bits()) {
        tlb_flush_page_by_mmuidx(cpu, addr, idxmap);
        return;
    }

    /* This should already be page aligned */
    d.addr = addr & TARGET_PAGE_MASK;
    d.len = len;
    d.idxmap = idxmap;
    d.bits = bits;

    tlb_flush_range_by_mmuidx_async_0(cpu, d);
}

void tlb_flush_page_bits_by_mmuidx(CPUState *cpu, vaddr addr,
                                   MMUIdxMap idxmap, unsigned bits)
{
    tlb_flush_range_by_mmuidx(cpu, addr, TARGET_PAGE_SIZE, idxmap, bits);
}

void tlb_flush_range_by_mmuidx_all_cpus_synced(CPUState *src_cpu,
                                               vaddr addr,
                                               vaddr len,
                                               MMUIdxMap idxmap,
                                               unsigned bits)
{
    TLBFlushRangeData d, *p;
    CPUState *dst_cpu;

    /* If no page bits are significant, this devolves to tlb_flush. */
    if (bits < TARGET_PAGE_BITS) {
        tlb_flush_by_mmuidx_all_cpus_synced(src_cpu, idxmap);
        return;
    }
    /*
     * If all bits are significant, and len is small,
     * this devolves to tlb_flush_page.
     */
    if (len <= TARGET_PAGE_SIZE && bits >= target_long_bits()) {
        tlb_flush_page_by_mmuidx_all_cpus_synced(src_cpu, addr, idxmap);
        return;
    }

    /* This should already be page aligned */
    d.addr = addr & TARGET_PAGE_MASK;
    d.len = len;
    d.idxmap = idxmap;
    d.bits = bits;

    if (tlb_shootdown_inline_run(src_cpu, TLB_SD_RANGE, d.addr, len, idxmap, bits)) {
        return;
    }
    if (tlb_shootdown_on(src_cpu)) {
        tlb_broadcast_synced(src_cpu, tlb_flush_range_by_mmuidx_async_1,
                             RUN_ON_CPU_HOST_PTR(g_memdup(&d, sizeof(d))), sizeof(d));
        return;
    }
    /* Allocate a separate data block for each destination cpu.  */
    CPU_FOREACH(dst_cpu) {
        if (tlb_flush_reaches(src_cpu, dst_cpu)) {
            p = g_memdup(&d, sizeof(d));
            async_run_on_cpu(dst_cpu, tlb_flush_range_by_mmuidx_async_1,
                             RUN_ON_CPU_HOST_PTR(p));
        }
    }

    p = g_memdup(&d, sizeof(d));
    async_safe_run_on_cpu(src_cpu, tlb_flush_range_by_mmuidx_async_1,
                          RUN_ON_CPU_HOST_PTR(p));
}

void tlb_flush_page_bits_by_mmuidx_all_cpus_synced(CPUState *src_cpu,
                                                   vaddr addr,
                                                   MMUIdxMap idxmap,
                                                   unsigned bits)
{
    tlb_flush_range_by_mmuidx_all_cpus_synced(src_cpu, addr, TARGET_PAGE_SIZE,
                                              idxmap, bits);
}


/*
 * The default broadcast (LT_TLB_SHOOTDOWN unset or "on"): each CPU the broadcast reaches gets the flush in a small
 * queue of its own and is sent out of its TB chain (icount_decr, as cpu_exit does without exit_request), so it runs
 * the flush at its next TB boundary in cpu_handle_interrupt, without leaving cpu_exec or taking the BQL. The issuer
 * flushes itself and waits in place until each target has run its queue that far or is outside guest code (it runs
 * the queue before its first TB). While it waits it runs its own queue, so two CPUs broadcasting at once don't wait
 * for each other. An issuer holding the BQL, which a target may be waiting for, takes the queued path instead.
 */

static void tlb_shootdown_run_op(CPUState *cpu, const TLBShootdownOp *o)
{
    switch (o->kind) {
    case TLB_SD_PAGE:
        tlb_flush_page_by_mmuidx_async_0(cpu, o->addr, o->idxmap);
        break;
    case TLB_SD_RANGE:
        tlb_flush_range_by_mmuidx_async_0(cpu, (TLBFlushRangeData){ o->addr, o->len, o->idxmap, o->bits });
        break;
    default:
        tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT(o->idxmap));
        break;
    }
}

void tlb_shootdown_service_slow(CPUState *cpu)
{
    struct TLBShootdownQueue *q = cpu->tlb_sd;
    TLBShootdownOp ops[TLB_SD_OPS];
    unsigned n;
    bool all;
    uint64_t upto;

    if (qatomic_load_acquire(&q->done) == qatomic_read(&q->enq)) {
        return;
    }
    qemu_spin_lock(&q->lock);
    n = q->n;
    all = q->all;
    upto = q->enq;
    memcpy(ops, q->op, n * sizeof(ops[0]));
    q->n = 0;
    q->all = false;
    qemu_spin_unlock(&q->lock);
    if (all) {
        tlb_flush_by_mmuidx_async_work(cpu, RUN_ON_CPU_HOST_INT(ALL_MMUIDX_BITS));
    } else {
        for (unsigned i = 0; i < n; i++) {
            tlb_shootdown_run_op(cpu, &ops[i]);
        }
    }
    qatomic_store_release(&q->done, upto);
}

static bool tlb_shootdown_inline_run(CPUState *src, int kind, vaddr addr, vaddr len, MMUIdxMap idxmap,
                                     unsigned bits)
{
    TLBShootdownOp o = { .addr = addr, .len = len, .idxmap = idxmap, .kind = kind, .bits = bits };
    CPUState *targets[TLB_SHOOTDOWN_MAX], *cpu;
    uint64_t tickets[TLB_SHOOTDOWN_MAX];
    int n = 0;
    int64_t t0;

    if (tlb_shootdown_mode() != 2 || current_cpu != src || bql_locked() || !src->tlb_sd) {
        return false;
    }
    t0 = it_acct_excl_on ? get_clock() : 0;
    CPU_FOREACH(cpu) {
        struct TLBShootdownQueue *q = cpu->tlb_sd;

        if (!tlb_flush_reaches(src, cpu) || !q || n == TLB_SHOOTDOWN_MAX) {
            if (cpu != src && q && n == TLB_SHOOTDOWN_MAX) {
                return false;   /* never: boards have fewer CPUs */
            }
            continue;
        }
        qemu_spin_lock(&q->lock);
        if (q->n < TLB_SD_OPS) {
            q->op[q->n++] = o;
        } else {
            q->all = true;   /* a full flush covers what didn't fit */
        }
        tickets[n] = ++q->enq;
        qemu_spin_unlock(&q->lock);
        targets[n++] = cpu;
        qatomic_set(&cpu->neg.icount_decr.u16.high, -1);
    }
    tlb_shootdown_run_op(src, &o);
    smp_mb();
    for (int i = 0; i < n; i++) {
        unsigned spins = 0;

        while (qatomic_load_acquire(&targets[i]->tlb_sd->done) < tickets[i] && qatomic_read(&targets[i]->running)) {
            tlb_shootdown_service_slow(src);
            if (++spins < 4000) {
                cpu_relax();
            } else {
                g_usleep(0);
            }
            smp_mb();
        }
    }
    if (t0) {
        it_acct_excl_note((void *)tlb_shootdown_inline_run, get_clock() - t0, 0);
    }
    return true;
}

/* update the TLBs so that writes to code in the virtual page 'addr'
   can be detected */
void tlb_protect_code(ram_addr_t ram_addr)
{
    physical_memory_test_and_clear_dirty(ram_addr & TARGET_PAGE_MASK,
                                         TARGET_PAGE_SIZE,
                                         DIRTY_MEMORY_CODE,
                                         NULL);
}

/* update the TLB so that writes in physical page 'phys_addr' are no longer
   tested for self modifying code */
void tlb_unprotect_code(ram_addr_t ram_addr)
{
    physical_memory_set_dirty_flag(ram_addr, DIRTY_MEMORY_CODE);
}


/*
 * Dirty write flag handling
 *
 * When the TCG code writes to a location it looks up the address in
 * the TLB and uses that data to compute the final address. If any of
 * the lower bits of the address are set then the slow path is forced.
 * There are a number of reasons to do this but for normal RAM the
 * most usual is detecting writes to code regions which may invalidate
 * generated code.
 *
 * Other vCPUs might be reading their TLBs during guest execution, so we update
 * te->addr_write with qatomic_set. We don't need to worry about this for
 * oversized guests as MTTCG is disabled for them.
 *
 * Called with tlb_c.lock held.
 */
static void tlb_reset_dirty_range_locked(CPUTLBEntryFull *full, CPUTLBEntry *ent,
                                         uintptr_t start, uintptr_t length)
{
    const uintptr_t addr = ent->addr_write;
    int flags = addr | full->slow_flags[MMU_DATA_STORE];

    flags &= TLB_INVALID_MASK | TLB_MMIO | TLB_DISCARD_WRITE | TLB_NOTDIRTY;
    if (flags == 0) {
        uintptr_t host = (addr & TARGET_PAGE_MASK) + ent->addend;
        if ((host - start) < length) {
            qatomic_set(&ent->addr_write, addr | TLB_NOTDIRTY);
        }
    }
}

/*
 * Called with tlb_c.lock held.
 * Called only from the vCPU context, i.e. the TLB's owner thread.
 */
static inline void copy_tlb_helper_locked(CPUTLBEntry *d, const CPUTLBEntry *s)
{
    *d = *s;
}

/* This is a cross vCPU call (i.e. another vCPU resetting the flags of
 * the target vCPU).
 * We must take tlb_c.lock to avoid racing with another vCPU update. The only
 * thing actually updated is the target TLB entry ->addr_write flags.
 */
static void tlb_bank_note_reset_locked(CPUState *cpu, uintptr_t start, uintptr_t length);

void tlb_reset_dirty(CPUState *cpu, uintptr_t start, uintptr_t length)
{
    int mmu_idx;

    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    if (unlikely(it_acct_on)) {
        it_acct_reset_dirty++;
    }
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        CPUTLBDesc *desc = &cpu->neg.tlb.d[mmu_idx];
        CPUTLBDescFast *fast = cpu_tlb_fast(cpu, mmu_idx);
        unsigned int n = tlb_n_entries(fast);
        unsigned int i;

        /* A clean index holds nothing (see tlb_flush_clean_skip): GL clears a texture's dirty bits at draws. */
        if (!((cpu->neg.tlb.c.dirty >> mmu_idx) & 1)) {
            continue;
        }

        for (i = 0; i < n; i++) {
            tlb_reset_dirty_range_locked(&desc->fulltlb[i], &fast->table[i],
                                         start, length);
        }

        for (i = 0; i < CPU_VTLB_SIZE; i++) {
            tlb_reset_dirty_range_locked(&desc->vfulltlb[i], &desc->vtable[i],
                                         start, length);
        }
    }
    tlb_bank_note_reset_locked(cpu, start, length);
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);
}

/*
 * Parked tables take the range when they come back (tlb_bank_catch_up_locked): walking them all here, from the
 * main loop or a draw, under the lock the vCPU needs, is what cost the first banks attempt. Too many: drop it.
 */
static void tlb_bank_note_reset_locked(CPUState *cpu, uintptr_t start, uintptr_t length)
{
    for (int b = 0; cpu->neg.tlb.c.banks && b < TLB_BANKS; b++) {
        TLBBank *bank = &cpu->neg.tlb.c.banks->b[b];
        unsigned r;

        if (!bank->used) {
            continue;
        }
        for (r = 0; r < bank->nreset; r++) {
            if (start >= bank->reset_start[r] && start + length <= bank->reset_start[r] + bank->reset_len[r]) {
                break;
            }
        }
        if (r < bank->nreset) {
            continue;
        }
        if (bank->nreset == TLB_BANK_RESETS) {
            bank->used = 0;
            continue;
        }
        bank->reset_start[bank->nreset] = start;
        bank->reset_len[bank->nreset++] = length;
    }
}

/* Whether host address `host` lies in one of n sorted disjoint (start, length) ranges. */
static bool tlb_ranges_hit(const uintptr_t *r, unsigned n, uintptr_t host)
{
    unsigned lo = 0, hi = n;

    while (lo < hi) {       /* the last range starting at or below host */
        unsigned mid = (lo + hi) / 2;

        if (r[2 * mid] <= host) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo && host - r[2 * (lo - 1)] < r[2 * (lo - 1) + 1];
}

static void tlb_reset_dirty_ranges_locked(CPUTLBEntryFull *full, CPUTLBEntry *ent, const uintptr_t *r, unsigned n)
{
    const uintptr_t addr = ent->addr_write;
    int flags = addr | full->slow_flags[MMU_DATA_STORE];

    flags &= TLB_INVALID_MASK | TLB_MMIO | TLB_DISCARD_WRITE | TLB_NOTDIRTY;
    if (flags == 0 && tlb_ranges_hit(r, n, (addr & TARGET_PAGE_MASK) + ent->addend)) {
        qatomic_set(&ent->addr_write, addr | TLB_NOTDIRTY);
    }
}

/*
 * tlb_reset_dirty for n sorted, disjoint ranges in one walk: GL's surface check clears a texture's dirty pages, a
 * physically scattered IOSurface being hundreds of runs, each of which was a walk of every TLB entry. Parked tables
 * take the span of the ranges (re-arming more than was cleared only costs a slow write).
 */
void tlb_reset_dirty_ranges(CPUState *cpu, const uintptr_t *r, unsigned n)
{
    if (!n) {
        return;
    }
    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    if (unlikely(it_acct_on)) {
        it_acct_reset_dirty++;
    }
    for (int mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        CPUTLBDesc *desc = &cpu->neg.tlb.d[mmu_idx];
        CPUTLBDescFast *fast = cpu_tlb_fast(cpu, mmu_idx);
        unsigned int entries = tlb_n_entries(fast);

        if (!((cpu->neg.tlb.c.dirty >> mmu_idx) & 1)) {
            continue;
        }
        for (unsigned int i = 0; i < entries; i++) {
            tlb_reset_dirty_ranges_locked(&desc->fulltlb[i], &fast->table[i], r, n);
        }
        for (unsigned int i = 0; i < CPU_VTLB_SIZE; i++) {
            tlb_reset_dirty_ranges_locked(&desc->vfulltlb[i], &desc->vtable[i], r, n);
        }
    }
    tlb_bank_note_reset_locked(cpu, r[0], r[2 * (n - 1)] + r[2 * (n - 1) + 1] - r[0]);
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);
}

/* Called with tlb_c.lock held */
static inline void tlb_set_dirty1_locked(CPUTLBEntry *tlb_entry,
                                         vaddr addr)
{
    if (tlb_entry->addr_write == (addr | TLB_NOTDIRTY)) {
        tlb_entry->addr_write = addr;
    }
}

/* update the TLB corresponding to virtual page vaddr
   so that it is no longer dirty */
static void tlb_set_dirty(CPUState *cpu, vaddr addr)
{
    int mmu_idx;

    assert_cpu_is_self(cpu);

    addr &= TARGET_PAGE_MASK;
    qemu_spin_lock(&cpu->neg.tlb.c.lock);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_set_dirty1_locked(tlb_entry(cpu, mmu_idx, addr), addr);
    }

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        int k;
        for (k = 0; k < CPU_VTLB_SIZE; k++) {
            tlb_set_dirty1_locked(&cpu->neg.tlb.d[mmu_idx].vtable[k], addr);
        }
    }
    qemu_spin_unlock(&cpu->neg.tlb.c.lock);
}

/* Our TLB does not support large pages, so remember the area covered by
   large pages and trigger a full TLB flush if these are invalidated.  */
static void tlb_add_large_page(CPUState *cpu, int mmu_idx,
                               vaddr addr, uint64_t size)
{
    vaddr lp_addr = cpu->neg.tlb.d[mmu_idx].large_page_addr;
    vaddr lp_mask = ~(size - 1);

    if (lp_addr == (vaddr)-1) {
        /* No previous large page.  */
        lp_addr = addr;
    } else {
        /* Extend the existing region to include the new page.
           This is a compromise between unnecessary flushes and
           the cost of maintaining a full variable size TLB.  */
        lp_mask &= cpu->neg.tlb.d[mmu_idx].large_page_mask;
        while (((lp_addr ^ addr) & lp_mask) != 0) {
            lp_mask <<= 1;
        }
    }
    cpu->neg.tlb.d[mmu_idx].large_page_addr = lp_addr & lp_mask;
    cpu->neg.tlb.d[mmu_idx].large_page_mask = lp_mask;
}

static inline void tlb_set_compare(CPUTLBEntryFull *full, CPUTLBEntry *ent,
                                   vaddr address, int flags,
                                   MMUAccessType access_type, bool enable)
{
    if (enable) {
        address |= flags & TLB_FLAGS_MASK;
        flags &= TLB_SLOW_FLAGS_MASK;
        if (flags) {
            address |= TLB_FORCE_SLOW;
        }
    } else {
        address = -1;
        flags = 0;
    }
    ent->addr_idx[access_type] = address;
    full->slow_flags[access_type] = flags;
}

/*
 * Add a new TLB entry. At most one entry for a given virtual address
 * is permitted. Only a single TARGET_PAGE_SIZE region is mapped, the
 * supplied size is only used by tlb_flush_page.
 *
 * Called from TCG-generated code, which is under an RCU read-side
 * critical section.
 */
void tlb_set_page_full(CPUState *cpu, int mmu_idx,
                       vaddr addr, CPUTLBEntryFull *full)
{
    CPUTLB *tlb = &cpu->neg.tlb;
    CPUTLBDesc *desc = &tlb->d[mmu_idx];
    MemoryRegionSection *section;
    unsigned int index, read_flags, write_flags;
    uintptr_t addend;
    CPUTLBEntry *te, tn;
    hwaddr iotlb, xlat, sz, paddr_page;
    vaddr addr_page;
    int asidx, wp_flags, prot;
    bool is_ram, is_romd;

    assert_cpu_is_self(cpu);

    if (unlikely(it_acct_on)) {
        it_acct_fills[mmu_idx & 31][addr >= 0x80000000]++;   /* by mmu_idx, low/high half */
    }
    if (full->lg_page_size <= TARGET_PAGE_BITS) {
        sz = TARGET_PAGE_SIZE;
    } else {
        sz = (hwaddr)1 << full->lg_page_size;
        tlb_add_large_page(cpu, mmu_idx, addr, sz);
    }
    addr_page = addr & TARGET_PAGE_MASK;
    paddr_page = full->phys_addr & TARGET_PAGE_MASK;

    prot = full->prot;
    asidx = cpu_asidx_from_attrs(cpu, full->attrs);
    section = address_space_translate_for_iotlb(cpu, asidx, paddr_page,
                                                &xlat, &sz, full->attrs, &prot);
    assert(sz >= TARGET_PAGE_SIZE);

    tlb_debug("vaddr=%016" VADDR_PRIx " paddr=0x" HWADDR_FMT_plx
              " prot=%x idx=%d\n",
              addr, full->phys_addr, prot, mmu_idx);

    read_flags = full->tlb_fill_flags;
    if (full->lg_page_size < TARGET_PAGE_BITS) {
        /* Repeat the MMU check and TLB fill on every access.  */
        read_flags |= TLB_INVALID_MASK;
    }

    is_ram = memory_region_is_ram(section->mr);
    is_romd = memory_region_is_romd(section->mr);

    if (is_ram || is_romd) {
        /* RAM and ROMD both have associated host memory. */
        addend = (uintptr_t)memory_region_get_ram_ptr(section->mr) + xlat;
    } else {
        /* I/O does not; force the host address to NULL. */
        addend = 0;
    }

    write_flags = read_flags;
    if (is_ram) {
        iotlb = memory_region_get_ram_addr(section->mr) + xlat;
        assert(!(iotlb & ~TARGET_PAGE_MASK));
        /*
         * Computing is_clean is expensive; avoid all that unless
         * the page is actually writable.
         */
        if (prot & PAGE_WRITE) {
            if (section->readonly) {
                write_flags |= TLB_DISCARD_WRITE;
            } else if (physical_memory_is_clean(iotlb)) {
                write_flags |= TLB_NOTDIRTY;
            }
        }
    } else {
        /* I/O or ROMD */
        iotlb = xlat;
        /*
         * Writes to romd devices must go through MMIO to enable write.
         * Reads to romd devices go through the ram_ptr found above,
         * but of course reads to I/O must go through MMIO.
         */
        write_flags |= TLB_MMIO;
        if (!is_romd) {
            read_flags = write_flags;
        }
    }

    wp_flags = cpu_watchpoint_address_matches(cpu, addr_page,
                                              TARGET_PAGE_SIZE);

    index = tlb_index(cpu, mmu_idx, addr_page);
    te = tlb_entry(cpu, mmu_idx, addr_page);

    /*
     * Hold the TLB lock for the rest of the function. We could acquire/release
     * the lock several times in the function, but it is faster to amortize the
     * acquisition cost by acquiring it just once. Note that this leads to
     * a longer critical section, but this is not a concern since the TLB lock
     * is unlikely to be contended.
     */
    qemu_spin_lock(&tlb->c.lock);

    /* Note that the tlb is no longer clean.  */
    tlb->c.dirty |= 1 << mmu_idx;

    /* Make sure there's no cached translation for the new page.  */
    tlb_flush_vtlb_page_locked(cpu, mmu_idx, addr_page);

    /*
     * Only evict the old entry to the victim tlb if it's for a
     * different page; otherwise just overwrite the stale data.
     */
    if (!tlb_hit_page_anyprot(te, addr_page) && !tlb_entry_is_empty(te)) {
        unsigned vidx = desc->vindex++ % CPU_VTLB_SIZE;
        CPUTLBEntry *tv = &desc->vtable[vidx];

        /* Evict the old entry into the victim tlb.  */
        copy_tlb_helper_locked(tv, te);
        desc->vfulltlb[vidx] = desc->fulltlb[index];
        tlb_n_used_entries_dec(cpu, mmu_idx);
    }

    /* refill the tlb */
    /*
     * When memory region is ram, iotlb contains a TARGET_PAGE_BITS
     * aligned ram_addr_t of the page base of the target RAM.
     * Otherwise, iotlb contains a TARGET_PAGE_BITS aligned
     * offset within section->mr of the page base (I/O, ROMD)
     *
     * We subtract addr_page (which is page aligned and thus won't
     * disturb the low bits) to give an offset which can be added to the
     * (non-page-aligned) vaddr of the eventual memory access to get
     * the MemoryRegion offset for the access. Note that the vaddr we
     * subtract here is that of the page base, and not the same as the
     * vaddr we add back in io_prepare()/get_page_addr_code().
     */
    desc->fulltlb[index] = *full;
    full = &desc->fulltlb[index];
    full->xlat_offset = iotlb - addr_page;
    full->section = section;
    full->phys_addr = paddr_page;

    /* Now calculate the new entry */
    tn.addend = addend - addr_page;

    tlb_set_compare(full, &tn, addr_page, read_flags,
                    MMU_INST_FETCH, prot & PAGE_EXEC);

    if (wp_flags & BP_MEM_READ) {
        read_flags |= TLB_WATCHPOINT;
    }
    tlb_set_compare(full, &tn, addr_page, read_flags,
                    MMU_DATA_LOAD, prot & PAGE_READ);

    if (prot & PAGE_WRITE_INV) {
        write_flags |= TLB_INVALID_MASK;
    }
    if (wp_flags & BP_MEM_WRITE) {
        write_flags |= TLB_WATCHPOINT;
    }
    tlb_set_compare(full, &tn, addr_page, write_flags,
                    MMU_DATA_STORE, prot & PAGE_WRITE);

    copy_tlb_helper_locked(te, &tn);
    tlb_n_used_entries_inc(cpu, mmu_idx);
    qemu_spin_unlock(&tlb->c.lock);
}

void tlb_set_page_with_attrs(CPUState *cpu, vaddr addr,
                             hwaddr paddr, MemTxAttrs attrs, int prot,
                             int mmu_idx, vaddr size)
{
    CPUTLBEntryFull full = {
        .phys_addr = paddr,
        .attrs = attrs,
        .prot = prot,
        .lg_page_size = ctz64(size)
    };

    assert(is_power_of_2(size));
    tlb_set_page_full(cpu, mmu_idx, addr, &full);
}

void tlb_set_page(CPUState *cpu, vaddr addr,
                  hwaddr paddr, int prot,
                  int mmu_idx, vaddr size)
{
    tlb_set_page_with_attrs(cpu, addr, paddr, MEMTXATTRS_UNSPECIFIED,
                            prot, mmu_idx, size);
}

/**
 * tlb_hit_page: return true if page aligned @addr is a hit against the
 * TLB entry @tlb_addr
 *
 * @addr: virtual address to test (must be page aligned)
 * @tlb_addr: TLB entry address (a CPUTLBEntry addr_read/write/code value)
 */
static inline bool tlb_hit_page(uint64_t tlb_addr, vaddr addr)
{
    return addr == (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK));
}

/**
 * tlb_hit: return true if @addr is a hit against the TLB entry @tlb_addr
 *
 * @addr: virtual address to test (need not be page aligned)
 * @tlb_addr: TLB entry address (a CPUTLBEntry addr_read/write/code value)
 */
static inline bool tlb_hit(uint64_t tlb_addr, vaddr addr)
{
    return tlb_hit_page(tlb_addr, addr & TARGET_PAGE_MASK);
}

/*
 * Note: tlb_fill_align() can trigger a resize of the TLB.
 * This means that all of the caller's prior references to the TLB table
 * (e.g. CPUTLBEntry pointers) must be discarded and looked up again
 * (e.g. via tlb_entry()).
 */
static bool tlb_fill_align(CPUState *cpu, vaddr addr, MMUAccessType type,
                           int mmu_idx, MemOp memop, int size,
                           bool probe, uintptr_t ra)
{
    const TCGCPUOps *ops = cpu->cc->tcg_ops;
    CPUTLBEntryFull full;

    if (ops->tlb_fill_align) {
        if (ops->tlb_fill_align(cpu, &full, addr, type, mmu_idx,
                                memop, size, probe, ra)) {
            tlb_set_page_full(cpu, mmu_idx, addr, &full);
            return true;
        }
    } else {
        /* Legacy behaviour is alignment before paging. */
        if (addr & ((1u << memop_alignment_bits(memop)) - 1)) {
            ops->do_unaligned_access(cpu, addr, type, mmu_idx, ra);
        }
        if (ops->tlb_fill(cpu, addr, size, type, mmu_idx, probe, ra)) {
            return true;
        }
    }
    assert(probe);
    return false;
}

static inline void cpu_unaligned_access(CPUState *cpu, vaddr addr,
                                        MMUAccessType access_type,
                                        int mmu_idx, uintptr_t retaddr)
{
    cpu->cc->tcg_ops->do_unaligned_access(cpu, addr, access_type,
                                          mmu_idx, retaddr);
}

static MemoryRegionSection *
io_prepare(hwaddr *out_offset, CPUState *cpu, CPUTLBEntryFull *full,
           vaddr addr, uintptr_t retaddr)
{
    MemoryRegionSection *section;
    hwaddr mr_offset;

    section = full->section;
    mr_offset = full->xlat_offset + addr;
    cpu->mem_io_pc = retaddr;
    if (!cpu->neg.can_do_io) {
        cpu_io_recompile(cpu, retaddr);
    }

    *out_offset = mr_offset;
    return section;
}

static void io_failed(CPUState *cpu, CPUTLBEntryFull *full, vaddr addr,
                      unsigned size, MMUAccessType access_type, int mmu_idx,
                      MemTxResult response, uintptr_t retaddr)
{
    if (!cpu->ignore_memory_transaction_failures
        && cpu->cc->tcg_ops->do_transaction_failed) {
        hwaddr physaddr = full->phys_addr | (addr & ~TARGET_PAGE_MASK);

        cpu->cc->tcg_ops->do_transaction_failed(cpu, physaddr, addr, size,
                                                access_type, mmu_idx,
                                                full->attrs, response, retaddr);
    }
}

/* Return true if ADDR is present in the victim tlb, and has been copied
   back to the main tlb.  */
static bool victim_tlb_hit(CPUState *cpu, size_t mmu_idx, size_t index,
                           MMUAccessType access_type, vaddr page)
{
    size_t vidx;

    assert_cpu_is_self(cpu);
    for (vidx = 0; vidx < CPU_VTLB_SIZE; ++vidx) {
        CPUTLBEntry *vtlb = &cpu->neg.tlb.d[mmu_idx].vtable[vidx];
        uint64_t cmp = tlb_read_idx(vtlb, access_type);

        if (cmp == page) {
            /* Found entry in victim tlb, swap tlb and iotlb.  */
            CPUTLBEntry tmptlb, *tlb = &cpu_tlb_fast(cpu, mmu_idx)->table[index];

            qemu_spin_lock(&cpu->neg.tlb.c.lock);
            copy_tlb_helper_locked(&tmptlb, tlb);
            copy_tlb_helper_locked(tlb, vtlb);
            copy_tlb_helper_locked(vtlb, &tmptlb);
            qemu_spin_unlock(&cpu->neg.tlb.c.lock);

            CPUTLBEntryFull *f1 = &cpu->neg.tlb.d[mmu_idx].fulltlb[index];
            CPUTLBEntryFull *f2 = &cpu->neg.tlb.d[mmu_idx].vfulltlb[vidx];
            CPUTLBEntryFull tmpf;
            tmpf = *f1; *f1 = *f2; *f2 = tmpf;
            return true;
        }
    }
    return false;
}

static void notdirty_write(CPUState *cpu, vaddr mem_vaddr, unsigned size,
                           CPUTLBEntryFull *full, uintptr_t retaddr)
{
    ram_addr_t ram_addr = mem_vaddr + full->xlat_offset;

    trace_memory_notdirty_write_access(mem_vaddr, ram_addr, size);

    if (!physical_memory_get_dirty_flag(ram_addr, DIRTY_MEMORY_CODE)) {
        tb_invalidate_phys_range_fast(cpu, ram_addr, size, retaddr);
    }

    /*
     * Set both VGA and migration bits for simplicity and to remove
     * the notdirty callback faster.
     */
    physical_memory_set_dirty_range(ram_addr, size, DIRTY_CLIENTS_NOCODE);

    /* We remove the notdirty callback only if the code has been flushed. */
    if (!physical_memory_is_clean(ram_addr)) {
        trace_memory_notdirty_set_dirty(mem_vaddr);
        tlb_set_dirty(cpu, mem_vaddr);
    }
}

static int probe_access_internal(CPUState *cpu, vaddr addr,
                                 int fault_size, MMUAccessType access_type,
                                 int mmu_idx, bool nonfault,
                                 void **phost, CPUTLBEntryFull **pfull,
                                 uintptr_t retaddr, bool check_mem_cbs)
{
    uintptr_t index = tlb_index(cpu, mmu_idx, addr);
    CPUTLBEntry *entry = tlb_entry(cpu, mmu_idx, addr);
    uint64_t tlb_addr = tlb_read_idx(entry, access_type);
    vaddr page_addr = addr & TARGET_PAGE_MASK;
    int flags = TLB_FLAGS_MASK & ~TLB_FORCE_SLOW;
    CPUTLBEntryFull *full;

    if (!tlb_hit_page(tlb_addr, page_addr)) {
        if (!victim_tlb_hit(cpu, mmu_idx, index, access_type, page_addr)) {
            if (!tlb_fill_align(cpu, addr, access_type, mmu_idx,
                                0, fault_size, nonfault, retaddr)) {
                /* Non-faulting page table read failed.  */
                *phost = NULL;
                *pfull = NULL;
                return TLB_INVALID_MASK;
            }

            /* TLB resize via tlb_fill_align may have moved the entry.  */
            index = tlb_index(cpu, mmu_idx, addr);
            entry = tlb_entry(cpu, mmu_idx, addr);

            /*
             * With PAGE_WRITE_INV, we set TLB_INVALID_MASK immediately,
             * to force the next access through tlb_fill_align.  We've just
             * called tlb_fill_align, so we know that this entry *is* valid.
             */
            flags &= ~TLB_INVALID_MASK;
        }
        tlb_addr = tlb_read_idx(entry, access_type);
    }
    flags &= tlb_addr;

    *pfull = full = &cpu->neg.tlb.d[mmu_idx].fulltlb[index];
    flags |= full->slow_flags[access_type];
    if (check_mem_cbs && cpu_plugin_mem_cbs_enabled(cpu)) {
        flags |= TLB_FORCE_SLOW;
    }

    *phost = (flags & ~(TLB_WATCHPOINT | TLB_NOTDIRTY | TLB_CHECK_ALIGNED)
              ? NULL
              : (void *)((uintptr_t)addr + entry->addend));
    return flags;
}

int probe_access_full(CPUArchState *env, vaddr addr, int size,
                      MMUAccessType access_type, int mmu_idx,
                      bool nonfault, void **phost, CPUTLBEntryFull **pfull,
                      uintptr_t retaddr)
{
    int flags = probe_access_internal(env_cpu(env), addr, size, access_type,
                                      mmu_idx, nonfault, phost, pfull, retaddr,
                                      true);

    /* Handle clean RAM pages.  */
    if (unlikely(flags & TLB_NOTDIRTY)) {
        int dirtysize = size == 0 ? 1 : size;
        notdirty_write(env_cpu(env), addr, dirtysize, *pfull, retaddr);
        flags &= ~TLB_NOTDIRTY;
    }

    return flags;
}

int probe_access_full_mmu(CPUArchState *env, vaddr addr, int size,
                          MMUAccessType access_type, int mmu_idx,
                          void **phost, CPUTLBEntryFull **pfull)
{
    void *discard_phost;
    CPUTLBEntryFull *discard_tlb;

    /* privately handle users that don't need full results */
    phost = phost ? phost : &discard_phost;
    pfull = pfull ? pfull : &discard_tlb;

    int flags = probe_access_internal(env_cpu(env), addr, size, access_type,
                                      mmu_idx, true, phost, pfull, 0, false);

    /* Handle clean RAM pages.  */
    if (unlikely(flags & TLB_NOTDIRTY)) {
        int dirtysize = size == 0 ? 1 : size;
        notdirty_write(env_cpu(env), addr, dirtysize, *pfull, 0);
        flags &= ~TLB_NOTDIRTY;
    }

    return flags;
}

int probe_access_flags(CPUArchState *env, vaddr addr, int size,
                       MMUAccessType access_type, int mmu_idx,
                       bool nonfault, void **phost, uintptr_t retaddr)
{
    CPUTLBEntryFull *full;
    int flags;

    g_assert(-(addr | TARGET_PAGE_MASK) >= size);

    flags = probe_access_internal(env_cpu(env), addr, size, access_type,
                                  mmu_idx, nonfault, phost, &full, retaddr,
                                  true);

    /* Handle clean RAM pages. */
    if (unlikely(flags & TLB_NOTDIRTY)) {
        int dirtysize = size == 0 ? 1 : size;
        notdirty_write(env_cpu(env), addr, dirtysize, full, retaddr);
        flags &= ~TLB_NOTDIRTY;
    }

    return flags;
}

void *probe_access(CPUArchState *env, vaddr addr, int size,
                   MMUAccessType access_type, int mmu_idx, uintptr_t retaddr)
{
    CPUTLBEntryFull *full;
    void *host;
    int flags;

    g_assert(-(addr | TARGET_PAGE_MASK) >= size);

    flags = probe_access_internal(env_cpu(env), addr, size, access_type,
                                  mmu_idx, false, &host, &full, retaddr,
                                  true);

    /* Per the interface, size == 0 merely faults the access. */
    if (size == 0) {
        return NULL;
    }

    if (unlikely(flags & (TLB_NOTDIRTY | TLB_WATCHPOINT))) {
        /* Handle watchpoints.  */
        if (flags & TLB_WATCHPOINT) {
            int wp_access = (access_type == MMU_DATA_STORE
                             ? BP_MEM_WRITE : BP_MEM_READ);
            cpu_check_watchpoint(env_cpu(env), addr, size,
                                 full->attrs, wp_access, retaddr);
        }

        /* Handle clean RAM pages.  */
        if (flags & TLB_NOTDIRTY) {
            notdirty_write(env_cpu(env), addr, size, full, retaddr);
        }
    }

    return host;
}

void *tlb_vaddr_to_host(CPUArchState *env, vaddr addr,
                        MMUAccessType access_type, int mmu_idx)
{
    CPUTLBEntryFull *full;
    void *host;
    int flags;

    flags = probe_access_internal(env_cpu(env), addr, 0, access_type,
                                  mmu_idx, true, &host, &full, 0, false);

    /* No combination of flags are expected by the caller. */
    return flags ? NULL : host;
}

/*
 * Return a ram_addr_t for the virtual address for execution.
 *
 * Return -1 if we can't translate and execute from an entire page
 * of RAM.  This will force us to execute by loading and translating
 * one insn at a time, without caching.
 *
 * NOTE: This function will trigger an exception if the page is
 * not executable.
 */
tb_page_addr_t get_page_addr_code_hostp(CPUArchState *env, vaddr addr,
                                        void **hostp)
{
    CPUTLBEntryFull *full;
    void *p;

    (void)probe_access_internal(env_cpu(env), addr, 1, MMU_INST_FETCH,
                                cpu_mmu_index(env_cpu(env), true), false,
                                hostp, &full, 0, false);

    p = *hostp;
    if (p == NULL) {
        return -1;
    }

    if (full->lg_page_size < TARGET_PAGE_BITS) {
        *hostp = NULL;
        return -1;
    }

    return qemu_ram_addr_from_host_nofail(p);
}

/* Load/store with atomicity primitives. */
#include "ldst_atomicity.c.inc"

#ifdef CONFIG_PLUGIN
/*
 * Perform a TLB lookup and populate the qemu_plugin_hwaddr structure.
 * This should be a hot path as we will have just looked this path up
 * in the softmmu lookup code (or helper). We don't handle re-fills or
 * checking the victim table. This is purely informational.
 *
 * The one corner case is i/o write, which can cause changes to the
 * address space.  Those changes, and the corresponding tlb flush,
 * should be delayed until the next TB, so even then this ought not fail.
 * But check, Just in Case.
 */
bool tlb_plugin_lookup(CPUState *cpu, vaddr addr, int mmu_idx,
                       bool is_store, struct qemu_plugin_hwaddr *data)
{
    CPUTLBEntry *tlbe = tlb_entry(cpu, mmu_idx, addr);
    uintptr_t index = tlb_index(cpu, mmu_idx, addr);
    MMUAccessType access_type = is_store ? MMU_DATA_STORE : MMU_DATA_LOAD;
    uint64_t tlb_addr = tlb_read_idx(tlbe, access_type);
    CPUTLBEntryFull *full;

    if (unlikely(!tlb_hit(tlb_addr, addr))) {
        return false;
    }

    full = &cpu->neg.tlb.d[mmu_idx].fulltlb[index];
    data->phys_addr = full->phys_addr | (addr & ~TARGET_PAGE_MASK);

    /* We must have an iotlb entry for MMIO */
    if (full->slow_flags[access_type] & TLB_MMIO) {
        MemoryRegionSection *section = full->section;
        data->is_io = true;
        data->mr = section->mr;
    } else {
        data->is_io = false;
        data->mr = NULL;
    }
    return true;
}
#endif

/*
 * Probe for a load/store operation.
 * Return the host address and into @flags.
 */

typedef struct MMULookupPageData {
    CPUTLBEntryFull *full;
    void *haddr;
    vaddr addr;
    int flags;
    int size;
} MMULookupPageData;

typedef struct MMULookupLocals {
    MMULookupPageData page[2];
    MemOp memop;
    int mmu_idx;
} MMULookupLocals;

/**
 * mmu_lookup1: translate one page
 * @cpu: generic cpu state
 * @data: lookup parameters
 * @memop: memory operation for the access, or 0
 * @mmu_idx: virtual address context
 * @access_type: load/store/code
 * @ra: return address into tcg generated code, or 0
 *
 * Resolve the translation for the one page at @data.addr, filling in
 * the rest of @data with the results.  If the translation fails,
 * tlb_fill_align will longjmp out.  Return true if the softmmu tlb for
 * @mmu_idx may have resized.
 */
static bool mmu_lookup1(CPUState *cpu, MMULookupPageData *data, MemOp memop,
                        int mmu_idx, MMUAccessType access_type, uintptr_t ra)
{
    vaddr addr = data->addr;
    uintptr_t index = tlb_index(cpu, mmu_idx, addr);
    CPUTLBEntry *entry = tlb_entry(cpu, mmu_idx, addr);
    uint64_t tlb_addr = tlb_read_idx(entry, access_type);
    bool maybe_resized = false;
    CPUTLBEntryFull *full;
    int flags;

    /* If the TLB entry is for a different page, reload and try again.  */
    if (!tlb_hit(tlb_addr, addr)) {
        if (!victim_tlb_hit(cpu, mmu_idx, index, access_type,
                            addr & TARGET_PAGE_MASK)) {
            tlb_fill_align(cpu, addr, access_type, mmu_idx,
                           memop, data->size, false, ra);
            maybe_resized = true;
            index = tlb_index(cpu, mmu_idx, addr);
            entry = tlb_entry(cpu, mmu_idx, addr);
        }
        tlb_addr = tlb_read_idx(entry, access_type) & ~TLB_INVALID_MASK;
    }

    full = &cpu->neg.tlb.d[mmu_idx].fulltlb[index];
    flags = tlb_addr & (TLB_FLAGS_MASK & ~TLB_FORCE_SLOW);
    flags |= full->slow_flags[access_type];

    if (likely(!maybe_resized)) {
        /* Alignment has not been checked by tlb_fill_align. */
        int a_bits = memop_tlb_alignment_bits(memop, flags & TLB_CHECK_ALIGNED);
        if (unlikely(addr & ((1 << a_bits) - 1))) {
            cpu_unaligned_access(cpu, addr, access_type, mmu_idx, ra);
        }
    }

    data->full = full;
    data->flags = flags;
    /* Compute haddr speculatively; depending on flags it might be invalid. */
    data->haddr = (void *)((uintptr_t)addr + entry->addend);

    return maybe_resized;
}

/**
 * mmu_watch_or_dirty
 * @cpu: generic cpu state
 * @data: lookup parameters
 * @access_type: load/store/code
 * @ra: return address into tcg generated code, or 0
 *
 * Trigger watchpoints for @data.addr:@data.size;
 * record writes to protected clean pages.
 */
static void mmu_watch_or_dirty(CPUState *cpu, MMULookupPageData *data,
                               MMUAccessType access_type, uintptr_t ra)
{
    CPUTLBEntryFull *full = data->full;
    vaddr addr = data->addr;
    int flags = data->flags;
    int size = data->size;

    /* On watchpoint hit, this will longjmp out.  */
    if (flags & TLB_WATCHPOINT) {
        int wp = access_type == MMU_DATA_STORE ? BP_MEM_WRITE : BP_MEM_READ;
        cpu_check_watchpoint(cpu, addr, size, full->attrs, wp, ra);
        flags &= ~TLB_WATCHPOINT;
    }

    /* Note that notdirty is only set for writes. */
    if (flags & TLB_NOTDIRTY) {
        notdirty_write(cpu, addr, size, full, ra);
        flags &= ~TLB_NOTDIRTY;
    }
    data->flags = flags;
}

/**
 * mmu_lookup: translate page(s)
 * @cpu: generic cpu state
 * @addr: virtual address
 * @oi: combined mmu_idx and MemOp
 * @ra: return address into tcg generated code, or 0
 * @access_type: load/store/code
 * @l: output result
 *
 * Resolve the translation for the page(s) beginning at @addr, for MemOp.size
 * bytes.  Return true if the lookup crosses a page boundary.
 */
static bool mmu_lookup(CPUState *cpu, vaddr addr, MemOpIdx oi,
                       uintptr_t ra, MMUAccessType type, MMULookupLocals *l)
{
    bool crosspage;
    vaddr last;
    int flags;

    l->memop = get_memop(oi);
    l->mmu_idx = get_mmuidx(oi);

    tcg_debug_assert(l->mmu_idx < NB_MMU_MODES);

    l->page[0].addr = addr;
    l->page[0].size = memop_size(l->memop);
    l->page[1].addr = 0;
    l->page[1].size = 0;

    /* Lookup and recognize exceptions from the first page. */
    mmu_lookup1(cpu, &l->page[0], l->memop, l->mmu_idx, type, ra);

    last = addr + l->page[0].size - 1;
    crosspage = (addr ^ last) & TARGET_PAGE_MASK;
    if (likely(!crosspage)) {
        flags = l->page[0].flags;
        if (unlikely(flags & (TLB_WATCHPOINT | TLB_NOTDIRTY))) {
            mmu_watch_or_dirty(cpu, &l->page[0], type, ra);
        }
        if (unlikely(flags & TLB_BSWAP)) {
            l->memop ^= MO_BSWAP;
        }
    } else {
        /* Finish compute of page crossing. */
        vaddr addr1 = last & TARGET_PAGE_MASK;
        int size0 = addr1 - addr;
        l->page[1].size = l->page[0].size - size0;
        l->page[0].size = size0;
        l->page[1].addr = cpu->cc->tcg_ops->pointer_wrap(cpu, l->mmu_idx,
                                                         addr1, addr);

        /*
         * Lookup and recognize exceptions from the second page.
         * If the lookup potentially resized the table, refresh the
         * first CPUTLBEntryFull pointer.
         */
        if (mmu_lookup1(cpu, &l->page[1], 0, l->mmu_idx, type, ra)) {
            uintptr_t index = tlb_index(cpu, l->mmu_idx, addr);
            l->page[0].full = &cpu->neg.tlb.d[l->mmu_idx].fulltlb[index];
        }

        flags = l->page[0].flags | l->page[1].flags;
        if (unlikely(flags & (TLB_WATCHPOINT | TLB_NOTDIRTY))) {
            mmu_watch_or_dirty(cpu, &l->page[0], type, ra);
            mmu_watch_or_dirty(cpu, &l->page[1], type, ra);
        }

        /*
         * Since target/sparc is the only user of TLB_BSWAP, and all
         * Sparc accesses are aligned, any treatment across two pages
         * would be arbitrary.  Refuse it until there's a use.
         */
        tcg_debug_assert((flags & TLB_BSWAP) == 0);
    }

    return crosspage;
}

/*
 * Probe for an atomic operation.  Do not allow unaligned operations,
 * or io operations to proceed.  Return the host address.
 */
static void *atomic_mmu_lookup(CPUState *cpu, vaddr addr, MemOpIdx oi,
                               int size, uintptr_t retaddr)
{
    uintptr_t mmu_idx = get_mmuidx(oi);
    MemOp mop = get_memop(oi);
    uintptr_t index;
    CPUTLBEntry *tlbe;
    vaddr tlb_addr;
    void *hostaddr;
    CPUTLBEntryFull *full;
    bool did_tlb_fill = false;

    tcg_debug_assert(mmu_idx < NB_MMU_MODES);

    /* Adjust the given return address.  */
    retaddr -= GETPC_ADJ;

    index = tlb_index(cpu, mmu_idx, addr);
    tlbe = tlb_entry(cpu, mmu_idx, addr);

    /* Check TLB entry and enforce page permissions.  */
    tlb_addr = tlb_addr_write(tlbe);
    if (!tlb_hit(tlb_addr, addr)) {
        if (!victim_tlb_hit(cpu, mmu_idx, index, MMU_DATA_STORE,
                            addr & TARGET_PAGE_MASK)) {
            tlb_fill_align(cpu, addr, MMU_DATA_STORE, mmu_idx,
                           mop, size, false, retaddr);
            did_tlb_fill = true;
            index = tlb_index(cpu, mmu_idx, addr);
            tlbe = tlb_entry(cpu, mmu_idx, addr);
        }
        tlb_addr = tlb_addr_write(tlbe) & ~TLB_INVALID_MASK;
    }

    /*
     * Let the guest notice RMW on a write-only page.
     * We have just verified that the page is writable.
     * Subpage lookups may have left TLB_INVALID_MASK set,
     * but addr_read will only be -1 if PAGE_READ was unset.
     */
    if (unlikely(tlbe->addr_read == -1)) {
        tlb_fill_align(cpu, addr, MMU_DATA_LOAD, mmu_idx,
                       0, size, false, retaddr);
        /*
         * Since we don't support reads and writes to different
         * addresses, and we do have the proper page loaded for
         * write, this shouldn't ever return.
         */
        g_assert_not_reached();
    }

    /* Enforce guest required alignment, if not handled by tlb_fill_align. */
    if (!did_tlb_fill && (addr & ((1 << memop_alignment_bits(mop)) - 1))) {
        cpu_unaligned_access(cpu, addr, MMU_DATA_STORE, mmu_idx, retaddr);
    }

    /* Enforce qemu required alignment.  */
    if (unlikely(addr & (size - 1))) {
        /*
         * We get here if guest alignment was not requested, or was not
         * enforced by cpu_unaligned_access or tlb_fill_align above.
         * We might widen the access and emulate, but for now
         * mark an exception and exit the cpu loop.
         */
        goto stop_the_world;
    }

    /* Finish collecting tlb flags for both read and write. */
    full = &cpu->neg.tlb.d[mmu_idx].fulltlb[index];
    tlb_addr |= tlbe->addr_read;
    tlb_addr &= TLB_FLAGS_MASK & ~TLB_FORCE_SLOW;
    tlb_addr |= full->slow_flags[MMU_DATA_STORE];
    tlb_addr |= full->slow_flags[MMU_DATA_LOAD];

    /* Notice an IO access or a needs-MMU-lookup access */
    if (unlikely(tlb_addr & (TLB_MMIO | TLB_DISCARD_WRITE))) {
        /* There's really nothing that can be done to
           support this apart from stop-the-world.  */
        goto stop_the_world;
    }

    hostaddr = (void *)((uintptr_t)addr + tlbe->addend);

    if (unlikely(tlb_addr & TLB_NOTDIRTY)) {
        notdirty_write(cpu, addr, size, full, retaddr);
    }

    if (unlikely(tlb_addr & TLB_WATCHPOINT)) {
        int wp_flags = 0;

        if (full->slow_flags[MMU_DATA_STORE] & TLB_WATCHPOINT) {
            wp_flags |= BP_MEM_WRITE;
        }
        if (full->slow_flags[MMU_DATA_LOAD] & TLB_WATCHPOINT) {
            wp_flags |= BP_MEM_READ;
        }
        cpu_check_watchpoint(cpu, addr, size,
                             full->attrs, wp_flags, retaddr);
    }

    return hostaddr;

 stop_the_world:
    cpu_loop_exit_atomic(cpu, retaddr);
}

/*
 * Load Helpers
 *
 * We support two different access types. SOFTMMU_CODE_ACCESS is
 * specifically for reading instructions from system memory. It is
 * called by the translation loop and in some helpers where the code
 * is disassembled. It shouldn't be called directly by guest code.
 *
 * For the benefit of TCG generated code, we want to avoid the
 * complication of ABI-specific return type promotion and always
 * return a value extended to the register size of the host. This is
 * tcg_target_long, except in the case of a 32-bit host and 64-bit
 * data, and for that we always have uint64_t.
 *
 * We don't bother with this widened value for SOFTMMU_CODE_ACCESS.
 */

/**
 * do_ld_mmio_beN:
 * @cpu: generic cpu state
 * @full: page parameters
 * @ret_be: accumulated data
 * @addr: virtual address
 * @size: number of bytes
 * @mmu_idx: virtual address context
 * @ra: return address into tcg generated code, or 0
 * Context: BQL held
 *
 * Load @size bytes from @addr, which is memory-mapped i/o.
 * The bytes are concatenated in big-endian order with @ret_be.
 */
static uint64_t int_ld_mmio_beN(CPUState *cpu, CPUTLBEntryFull *full,
                                uint64_t ret_be, vaddr addr, int size,
                                int mmu_idx, MMUAccessType type, uintptr_t ra,
                                MemoryRegion *mr, hwaddr mr_offset)
{
    do {
        MemOp this_mop;
        unsigned this_size;
        uint64_t val;
        MemTxResult r;

        /* Read aligned pieces up to 8 bytes. */
        this_mop = ctz32(size | (int)addr | 8);
        this_size = 1 << this_mop;
        this_mop |= MO_BE;

        r = memory_region_dispatch_read(mr, mr_offset, &val,
                                        this_mop, full->attrs);
        if (unlikely(r != MEMTX_OK)) {
            io_failed(cpu, full, addr, this_size, type, mmu_idx, r, ra);
        }
        if (this_size == 8) {
            return val;
        }

        ret_be = (ret_be << (this_size * 8)) | val;
        addr += this_size;
        mr_offset += this_size;
        size -= this_size;
    } while (size);

    return ret_be;
}

/*
 * As on the physmem path, a region that opted in with
 * memory_region_enable_lockless_io is dispatched without the BQL; its
 * callbacks take it where they need it.
 */
typedef struct ItAcctMMIOScope {
    void *previous;
} ItAcctMMIOScope;

static inline ItAcctMMIOScope it_acct_mmio_enter(MemoryRegion *mr)
{
    ItAcctMMIOScope scope = { it_acct_mmio_mr };

    it_acct_mmio_mr = mr;
    return scope;
}

static inline void it_acct_mmio_leave(ItAcctMMIOScope *scope)
{
    it_acct_mmio_mr = scope->previous;
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(ItAcctMMIOScope, it_acct_mmio_leave)

/*
 * LT_MMIO_HIST=on: count guest MMIO accesses by region, 4-byte offset and direction (diagnostics, racy counts), to
 * find the registers a guest hits often enough that taking the BQL for them matters. The accounting dump prints them.
 */
struct it_mmio_hist { void *mr; uint32_t off; uint64_t n; char *name; } it_mmio_hist[4096];
int it_mmio_hist_on = -1;

static void it_mmio_hist_note(MemoryRegion *mr, hwaddr off, bool write)
{
    uint32_t key = (uint32_t)(off & ~(hwaddr)3) | write;
    uint32_t h = (uint32_t)(((uintptr_t)mr >> 4) * 2654435761u ^ key * 40503u) & 4095;

    for (int i = 0; i < 4096; i++, h = (h + 1) & 4095) {
        struct it_mmio_hist *e = &it_mmio_hist[h];
        if (e->mr == mr && e->off == key) {
            qatomic_set(&e->n, e->n + 1);
            return;
        }
        if (!e->mr) {
            /* Named now: some regions are transient and freed later. */
            e->name = g_strdup_printf("%s@0x%" PRIx64, memory_region_name(mr) ?: "?", (uint64_t)mr->addr);
            e->off = key;
            e->n = 1;
            qatomic_store_release(&e->mr, mr);
            return;
        }
    }
}

static inline void it_mmio_hist_maybe(MemoryRegion *mr, hwaddr off, bool write)
{
    if (unlikely(it_mmio_hist_on)) {
        if (it_mmio_hist_on < 0) {
            const char *v = getenv("LT_MMIO_HIST");
            it_mmio_hist_on = v && !strcmp(v, "on");
            if (!it_mmio_hist_on) {
                return;
            }
        }
        it_mmio_hist_note(mr, off, write);
    }
}

/* Lockless callbacks may acquire the BQL themselves. Keep their region for
 * that acquisition, then restore it even when no acquisition consumed it.
 * Otherwise a timer-counter read can mislabel a subsequent interrupt lock. */
#define MMIO_BQL_LOCK_GUARD(mr) \
    g_auto(ItAcctMMIOScope) _it_acct_mmio_scope = it_acct_mmio_enter(mr); \
    g_autoptr(BQLLockAuto) _bql_lock_auto __attribute__((unused)) \
        = (mr)->lockless_io ? NULL : bql_auto_lock(__FILE__, __LINE__)

static uint64_t do_ld_mmio_beN(CPUState *cpu, CPUTLBEntryFull *full,
                               uint64_t ret_be, vaddr addr, int size,
                               int mmu_idx, MMUAccessType type, uintptr_t ra)
{
    MemoryRegionSection *section;
    MemoryRegion *mr;
    hwaddr mr_offset;

    tcg_debug_assert(size > 0 && size <= 8);

    section = io_prepare(&mr_offset, cpu, full, addr, ra);
    mr = section->mr;

    MMIO_BQL_LOCK_GUARD(mr);
    it_mmio_hist_maybe(mr, mr_offset, false);
    return int_ld_mmio_beN(cpu, full, ret_be, addr, size, mmu_idx,
                           type, ra, mr, mr_offset);
}

static Int128 do_ld16_mmio_beN(CPUState *cpu, CPUTLBEntryFull *full,
                               uint64_t ret_be, vaddr addr, int size,
                               int mmu_idx, uintptr_t ra)
{
    MemoryRegionSection *section;
    MemoryRegion *mr;
    hwaddr mr_offset;
    uint64_t a, b;

    tcg_debug_assert(size > 8 && size <= 16);

    section = io_prepare(&mr_offset, cpu, full, addr, ra);
    mr = section->mr;

    MMIO_BQL_LOCK_GUARD(mr);
    it_mmio_hist_maybe(mr, mr_offset, false);
    a = int_ld_mmio_beN(cpu, full, ret_be, addr, size - 8, mmu_idx,
                        MMU_DATA_LOAD, ra, mr, mr_offset);
    b = int_ld_mmio_beN(cpu, full, ret_be, addr + size - 8, 8, mmu_idx,
                        MMU_DATA_LOAD, ra, mr, mr_offset + size - 8);
    return int128_make128(b, a);
}

/**
 * do_ld_bytes_beN
 * @p: translation parameters
 * @ret_be: accumulated data
 *
 * Load @p->size bytes from @p->haddr, which is RAM.
 * The bytes to concatenated in big-endian order with @ret_be.
 */
static uint64_t do_ld_bytes_beN(MMULookupPageData *p, uint64_t ret_be)
{
    uint8_t *haddr = p->haddr;
    int i, size = p->size;

    for (i = 0; i < size; i++) {
        ret_be = (ret_be << 8) | haddr[i];
    }
    return ret_be;
}

/**
 * do_ld_parts_beN
 * @p: translation parameters
 * @ret_be: accumulated data
 *
 * As do_ld_bytes_beN, but atomically on each aligned part.
 */
static uint64_t do_ld_parts_beN(MMULookupPageData *p, uint64_t ret_be)
{
    void *haddr = p->haddr;
    int size = p->size;

    do {
        uint64_t x;
        int n;

        /*
         * Find minimum of alignment and size.
         * This is slightly stronger than required by MO_ATOM_SUBALIGN, which
         * would have only checked the low bits of addr|size once at the start,
         * but is just as easy.
         */
        switch (((uintptr_t)haddr | size) & 7) {
        case 4:
            x = cpu_to_be32(load_atomic4(haddr));
            ret_be = (ret_be << 32) | x;
            n = 4;
            break;
        case 2:
        case 6:
            x = cpu_to_be16(load_atomic2(haddr));
            ret_be = (ret_be << 16) | x;
            n = 2;
            break;
        default:
            x = *(uint8_t *)haddr;
            ret_be = (ret_be << 8) | x;
            n = 1;
            break;
        case 0:
            g_assert_not_reached();
        }
        haddr += n;
        size -= n;
    } while (size != 0);
    return ret_be;
}

/**
 * do_ld_parts_be8
 * @p: translation parameters
 * @ret_be: accumulated data
 *
 * As do_ld_bytes_beN, but with one atomic load.
 * Eight aligned bytes are guaranteed to cover the load.
 */
static uint64_t do_ld_whole_be8(CPUState *cpu, uintptr_t ra,
                                MMULookupPageData *p, uint64_t ret_be)
{
    int o = p->addr & 7;
    uint64_t x = load_atomic8(p->haddr - o);

    x = cpu_to_be64(x);
    x <<= o * 8;
    x >>= (8 - p->size) * 8;
    return (ret_be << (p->size * 8)) | x;
}

/**
 * do_ld_parts_be16
 * @p: translation parameters
 * @ret_be: accumulated data
 *
 * As do_ld_bytes_beN, but with one atomic load.
 * 16 aligned bytes are guaranteed to cover the load.
 */
static Int128 do_ld_whole_be16(CPUState *cpu, uintptr_t ra,
                               MMULookupPageData *p, uint64_t ret_be)
{
    int o = p->addr & 15;
    Int128 x, y = load_atomic16_or_exit(cpu, ra, p->haddr - o);
    int size = p->size;

    if (!HOST_BIG_ENDIAN) {
        y = bswap128(y);
    }
    y = int128_lshift(y, o * 8);
    y = int128_urshift(y, (16 - size) * 8);
    x = int128_make64(ret_be);
    x = int128_lshift(x, size * 8);
    return int128_or(x, y);
}

/*
 * Wrapper for the above.
 */
static uint64_t do_ld_beN(CPUState *cpu, MMULookupPageData *p,
                          uint64_t ret_be, int mmu_idx, MMUAccessType type,
                          MemOp mop, uintptr_t ra)
{
    MemOp atom;
    unsigned tmp, half_size;

    if (unlikely(p->flags & TLB_MMIO)) {
        return do_ld_mmio_beN(cpu, p->full, ret_be, p->addr, p->size,
                              mmu_idx, type, ra);
    }

    /*
     * It is a given that we cross a page and therefore there is no
     * atomicity for the load as a whole, but subobjects may need attention.
     */
    atom = mop & MO_ATOM_MASK;
    switch (atom) {
    case MO_ATOM_SUBALIGN:
        return do_ld_parts_beN(p, ret_be);

    case MO_ATOM_IFALIGN_PAIR:
    case MO_ATOM_WITHIN16_PAIR:
        tmp = mop & MO_SIZE;
        tmp = tmp ? tmp - 1 : 0;
        half_size = 1 << tmp;
        if (atom == MO_ATOM_IFALIGN_PAIR
            ? p->size == half_size
            : p->size >= half_size) {
            return do_ld_whole_be8(cpu, ra, p, ret_be);
        }
        /* fall through */

    case MO_ATOM_IFALIGN:
    case MO_ATOM_WITHIN16:
    case MO_ATOM_NONE:
        return do_ld_bytes_beN(p, ret_be);

    default:
        g_assert_not_reached();
    }
}

/*
 * Wrapper for the above, for 8 < size < 16.
 */
static Int128 do_ld16_beN(CPUState *cpu, MMULookupPageData *p,
                          uint64_t a, int mmu_idx, MemOp mop, uintptr_t ra)
{
    int size = p->size;
    uint64_t b;
    MemOp atom;

    if (unlikely(p->flags & TLB_MMIO)) {
        return do_ld16_mmio_beN(cpu, p->full, a, p->addr, size, mmu_idx, ra);
    }

    /*
     * It is a given that we cross a page and therefore there is no
     * atomicity for the load as a whole, but subobjects may need attention.
     */
    atom = mop & MO_ATOM_MASK;
    switch (atom) {
    case MO_ATOM_SUBALIGN:
        p->size = size - 8;
        a = do_ld_parts_beN(p, a);
        p->haddr += size - 8;
        p->size = 8;
        b = do_ld_parts_beN(p, 0);
        break;

    case MO_ATOM_WITHIN16_PAIR:
        /* Since size > 8, this is the half that must be atomic. */
        return do_ld_whole_be16(cpu, ra, p, a);

    case MO_ATOM_IFALIGN_PAIR:
        /*
         * Since size > 8, both halves are misaligned,
         * and so neither is atomic.
         */
    case MO_ATOM_IFALIGN:
    case MO_ATOM_WITHIN16:
    case MO_ATOM_NONE:
        p->size = size - 8;
        a = do_ld_bytes_beN(p, a);
        b = ldq_be_p(p->haddr + size - 8);
        break;

    default:
        g_assert_not_reached();
    }

    return int128_make128(b, a);
}

static uint8_t do_ld_1(CPUState *cpu, MMULookupPageData *p, int mmu_idx,
                       MMUAccessType type, uintptr_t ra)
{
    if (unlikely(p->flags & TLB_MMIO)) {
        return do_ld_mmio_beN(cpu, p->full, 0, p->addr, 1, mmu_idx, type, ra);
    } else {
        return *(uint8_t *)p->haddr;
    }
}

static uint16_t do_ld_2(CPUState *cpu, MMULookupPageData *p, int mmu_idx,
                        MMUAccessType type, MemOp memop, uintptr_t ra)
{
    uint16_t ret;

    if (unlikely(p->flags & TLB_MMIO)) {
        ret = do_ld_mmio_beN(cpu, p->full, 0, p->addr, 2, mmu_idx, type, ra);
        if ((memop & MO_BSWAP) == MO_LE) {
            ret = bswap16(ret);
        }
    } else {
        /* Perform the load host endian, then swap if necessary. */
        ret = load_atom_2(cpu, ra, p->haddr, memop);
        if (memop & MO_BSWAP) {
            ret = bswap16(ret);
        }
    }
    return ret;
}

static uint32_t do_ld_4(CPUState *cpu, MMULookupPageData *p, int mmu_idx,
                        MMUAccessType type, MemOp memop, uintptr_t ra)
{
    uint32_t ret;

    if (unlikely(p->flags & TLB_MMIO)) {
        ret = do_ld_mmio_beN(cpu, p->full, 0, p->addr, 4, mmu_idx, type, ra);
        if ((memop & MO_BSWAP) == MO_LE) {
            ret = bswap32(ret);
        }
    } else {
        /* Perform the load host endian. */
        ret = load_atom_4(cpu, ra, p->haddr, memop);
        if (memop & MO_BSWAP) {
            ret = bswap32(ret);
        }
    }
    return ret;
}

static uint64_t do_ld_8(CPUState *cpu, MMULookupPageData *p, int mmu_idx,
                        MMUAccessType type, MemOp memop, uintptr_t ra)
{
    uint64_t ret;

    if (unlikely(p->flags & TLB_MMIO)) {
        ret = do_ld_mmio_beN(cpu, p->full, 0, p->addr, 8, mmu_idx, type, ra);
        if ((memop & MO_BSWAP) == MO_LE) {
            ret = bswap64(ret);
        }
    } else {
        /* Perform the load host endian. */
        ret = load_atom_8(cpu, ra, p->haddr, memop);
        if (memop & MO_BSWAP) {
            ret = bswap64(ret);
        }
    }
    return ret;
}

static uint8_t do_ld1_mmu(CPUState *cpu, vaddr addr, MemOpIdx oi,
                          uintptr_t ra, MMUAccessType access_type)
{
    MMULookupLocals l;
    bool crosspage;

    cpu_req_mo(cpu, TCG_MO_LD_LD | TCG_MO_ST_LD);
    crosspage = mmu_lookup(cpu, addr, oi, ra, access_type, &l);
    tcg_debug_assert(!crosspage);

    return do_ld_1(cpu, &l.page[0], l.mmu_idx, access_type, ra);
}

static uint16_t do_ld2_mmu(CPUState *cpu, vaddr addr, MemOpIdx oi,
                           uintptr_t ra, MMUAccessType access_type)
{
    MMULookupLocals l;
    bool crosspage;
    uint16_t ret;
    uint8_t a, b;

    cpu_req_mo(cpu, TCG_MO_LD_LD | TCG_MO_ST_LD);
    crosspage = mmu_lookup(cpu, addr, oi, ra, access_type, &l);
    if (likely(!crosspage)) {
        return do_ld_2(cpu, &l.page[0], l.mmu_idx, access_type, l.memop, ra);
    }

    a = do_ld_1(cpu, &l.page[0], l.mmu_idx, access_type, ra);
    b = do_ld_1(cpu, &l.page[1], l.mmu_idx, access_type, ra);

    if ((l.memop & MO_BSWAP) == MO_LE) {
        ret = a | (b << 8);
    } else {
        ret = b | (a << 8);
    }
    return ret;
}

static uint32_t do_ld4_mmu(CPUState *cpu, vaddr addr, MemOpIdx oi,
                           uintptr_t ra, MMUAccessType access_type)
{
    MMULookupLocals l;
    bool crosspage;
    uint32_t ret;

    cpu_req_mo(cpu, TCG_MO_LD_LD | TCG_MO_ST_LD);
    crosspage = mmu_lookup(cpu, addr, oi, ra, access_type, &l);
    if (likely(!crosspage)) {
        return do_ld_4(cpu, &l.page[0], l.mmu_idx, access_type, l.memop, ra);
    }

    ret = do_ld_beN(cpu, &l.page[0], 0, l.mmu_idx, access_type, l.memop, ra);
    ret = do_ld_beN(cpu, &l.page[1], ret, l.mmu_idx, access_type, l.memop, ra);
    if ((l.memop & MO_BSWAP) == MO_LE) {
        ret = bswap32(ret);
    }
    return ret;
}

static uint64_t do_ld8_mmu(CPUState *cpu, vaddr addr, MemOpIdx oi,
                           uintptr_t ra, MMUAccessType access_type)
{
    MMULookupLocals l;
    bool crosspage;
    uint64_t ret;

    cpu_req_mo(cpu, TCG_MO_LD_LD | TCG_MO_ST_LD);
    crosspage = mmu_lookup(cpu, addr, oi, ra, access_type, &l);
    if (likely(!crosspage)) {
        return do_ld_8(cpu, &l.page[0], l.mmu_idx, access_type, l.memop, ra);
    }

    ret = do_ld_beN(cpu, &l.page[0], 0, l.mmu_idx, access_type, l.memop, ra);
    ret = do_ld_beN(cpu, &l.page[1], ret, l.mmu_idx, access_type, l.memop, ra);
    if ((l.memop & MO_BSWAP) == MO_LE) {
        ret = bswap64(ret);
    }
    return ret;
}

static Int128 do_ld16_mmu(CPUState *cpu, vaddr addr,
                          MemOpIdx oi, uintptr_t ra)
{
    MMULookupLocals l;
    bool crosspage;
    uint64_t a, b;
    Int128 ret;
    int first;

    cpu_req_mo(cpu, TCG_MO_LD_LD | TCG_MO_ST_LD);
    crosspage = mmu_lookup(cpu, addr, oi, ra, MMU_DATA_LOAD, &l);
    if (likely(!crosspage)) {
        if (unlikely(l.page[0].flags & TLB_MMIO)) {
            ret = do_ld16_mmio_beN(cpu, l.page[0].full, 0, addr, 16,
                                   l.mmu_idx, ra);
            if ((l.memop & MO_BSWAP) == MO_LE) {
                ret = bswap128(ret);
            }
        } else {
            /* Perform the load host endian. */
            ret = load_atom_16(cpu, ra, l.page[0].haddr, l.memop);
            if (l.memop & MO_BSWAP) {
                ret = bswap128(ret);
            }
        }
        return ret;
    }

    first = l.page[0].size;
    if (first == 8) {
        MemOp mop8 = (l.memop & ~MO_SIZE) | MO_64;

        a = do_ld_8(cpu, &l.page[0], l.mmu_idx, MMU_DATA_LOAD, mop8, ra);
        b = do_ld_8(cpu, &l.page[1], l.mmu_idx, MMU_DATA_LOAD, mop8, ra);
        if ((mop8 & MO_BSWAP) == MO_LE) {
            ret = int128_make128(a, b);
        } else {
            ret = int128_make128(b, a);
        }
        return ret;
    }

    if (first < 8) {
        a = do_ld_beN(cpu, &l.page[0], 0, l.mmu_idx,
                      MMU_DATA_LOAD, l.memop, ra);
        ret = do_ld16_beN(cpu, &l.page[1], a, l.mmu_idx, l.memop, ra);
    } else {
        ret = do_ld16_beN(cpu, &l.page[0], 0, l.mmu_idx, l.memop, ra);
        b = int128_getlo(ret);
        ret = int128_lshift(ret, l.page[1].size * 8);
        a = int128_gethi(ret);
        b = do_ld_beN(cpu, &l.page[1], b, l.mmu_idx,
                      MMU_DATA_LOAD, l.memop, ra);
        ret = int128_make128(b, a);
    }
    if ((l.memop & MO_BSWAP) == MO_LE) {
        ret = bswap128(ret);
    }
    return ret;
}

/*
 * Store Helpers
 */

/**
 * do_st_mmio_leN:
 * @cpu: generic cpu state
 * @full: page parameters
 * @val_le: data to store
 * @addr: virtual address
 * @size: number of bytes
 * @mmu_idx: virtual address context
 * @ra: return address into tcg generated code, or 0
 * Context: BQL held
 *
 * Store @size bytes at @addr, which is memory-mapped i/o.
 * The bytes to store are extracted in little-endian order from @val_le;
 * return the bytes of @val_le beyond @p->size that have not been stored.
 */
static uint64_t int_st_mmio_leN(CPUState *cpu, CPUTLBEntryFull *full,
                                uint64_t val_le, vaddr addr, int size,
                                int mmu_idx, uintptr_t ra,
                                MemoryRegion *mr, hwaddr mr_offset)
{
    do {
        MemOp this_mop;
        unsigned this_size;
        MemTxResult r;

        /* Store aligned pieces up to 8 bytes. */
        this_mop = ctz32(size | (int)addr | 8);
        this_size = 1 << this_mop;
        this_mop |= MO_LE;

        r = memory_region_dispatch_write(mr, mr_offset, val_le,
                                         this_mop, full->attrs);
        if (unlikely(r != MEMTX_OK)) {
            io_failed(cpu, full, addr, this_size, MMU_DATA_STORE,
                      mmu_idx, r, ra);
        }
        if (this_size == 8) {
            return 0;
        }

        val_le >>= this_size * 8;
        addr += this_size;
        mr_offset += this_size;
        size -= this_size;
    } while (size);

    return val_le;
}

static uint64_t do_st_mmio_leN(CPUState *cpu, CPUTLBEntryFull *full,
                               uint64_t val_le, vaddr addr, int size,
                               int mmu_idx, uintptr_t ra)
{
    MemoryRegionSection *section;
    hwaddr mr_offset;
    MemoryRegion *mr;

    tcg_debug_assert(size > 0 && size <= 8);

    section = io_prepare(&mr_offset, cpu, full, addr, ra);
    mr = section->mr;

    MMIO_BQL_LOCK_GUARD(mr);
    it_mmio_hist_maybe(mr, mr_offset, true);
    return int_st_mmio_leN(cpu, full, val_le, addr, size, mmu_idx,
                           ra, mr, mr_offset);
}

static uint64_t do_st16_mmio_leN(CPUState *cpu, CPUTLBEntryFull *full,
                                 Int128 val_le, vaddr addr, int size,
                                 int mmu_idx, uintptr_t ra)
{
    MemoryRegionSection *section;
    MemoryRegion *mr;
    hwaddr mr_offset;

    tcg_debug_assert(size > 8 && size <= 16);

    section = io_prepare(&mr_offset, cpu, full, addr, ra);
    mr = section->mr;

    MMIO_BQL_LOCK_GUARD(mr);
    it_mmio_hist_maybe(mr, mr_offset, true);
    int_st_mmio_leN(cpu, full, int128_getlo(val_le), addr, 8,
                    mmu_idx, ra, mr, mr_offset);
    return int_st_mmio_leN(cpu, full, int128_gethi(val_le), addr + 8,
                           size - 8, mmu_idx, ra, mr, mr_offset + 8);
}

/*
 * Wrapper for the above.
 */
static uint64_t do_st_leN(CPUState *cpu, MMULookupPageData *p,
                          uint64_t val_le, int mmu_idx,
                          MemOp mop, uintptr_t ra)
{
    MemOp atom;
    unsigned tmp, half_size;

    if (unlikely(p->flags & TLB_MMIO)) {
        return do_st_mmio_leN(cpu, p->full, val_le, p->addr,
                              p->size, mmu_idx, ra);
    } else if (unlikely(p->flags & TLB_DISCARD_WRITE)) {
        return val_le >> (p->size * 8);
    }

    /*
     * It is a given that we cross a page and therefore there is no atomicity
     * for the store as a whole, but subobjects may need attention.
     */
    atom = mop & MO_ATOM_MASK;
    switch (atom) {
    case MO_ATOM_SUBALIGN:
        return store_parts_leN(p->haddr, p->size, val_le);

    case MO_ATOM_IFALIGN_PAIR:
    case MO_ATOM_WITHIN16_PAIR:
        tmp = mop & MO_SIZE;
        tmp = tmp ? tmp - 1 : 0;
        half_size = 1 << tmp;
        if (atom == MO_ATOM_IFALIGN_PAIR
            ? p->size == half_size
            : p->size >= half_size) {
            return store_whole_le8(p->haddr, p->size, val_le);
        }
        /* fall through */

    case MO_ATOM_IFALIGN:
    case MO_ATOM_WITHIN16:
    case MO_ATOM_NONE:
        return store_bytes_leN(p->haddr, p->size, val_le);

    default:
        g_assert_not_reached();
    }
}

/*
 * Wrapper for the above, for 8 < size < 16.
 */
static uint64_t do_st16_leN(CPUState *cpu, MMULookupPageData *p,
                            Int128 val_le, int mmu_idx,
                            MemOp mop, uintptr_t ra)
{
    int size = p->size;
    MemOp atom;

    if (unlikely(p->flags & TLB_MMIO)) {
        return do_st16_mmio_leN(cpu, p->full, val_le, p->addr,
                                size, mmu_idx, ra);
    } else if (unlikely(p->flags & TLB_DISCARD_WRITE)) {
        return int128_gethi(val_le) >> ((size - 8) * 8);
    }

    /*
     * It is a given that we cross a page and therefore there is no atomicity
     * for the store as a whole, but subobjects may need attention.
     */
    atom = mop & MO_ATOM_MASK;
    switch (atom) {
    case MO_ATOM_SUBALIGN:
        store_parts_leN(p->haddr, 8, int128_getlo(val_le));
        return store_parts_leN(p->haddr + 8, p->size - 8,
                               int128_gethi(val_le));

    case MO_ATOM_WITHIN16_PAIR:
        /* Since size > 8, this is the half that must be atomic. */
        if (!HAVE_CMPXCHG128) {
            cpu_loop_exit_atomic(cpu, ra);
        }
        return store_whole_le16(p->haddr, p->size, val_le);

    case MO_ATOM_IFALIGN_PAIR:
        /*
         * Since size > 8, both halves are misaligned,
         * and so neither is atomic.
         */
    case MO_ATOM_IFALIGN:
    case MO_ATOM_WITHIN16:
    case MO_ATOM_NONE:
        stq_le_p(p->haddr, int128_getlo(val_le));
        return store_bytes_leN(p->haddr + 8, p->size - 8,
                               int128_gethi(val_le));

    default:
        g_assert_not_reached();
    }
}

static void do_st_1(CPUState *cpu, MMULookupPageData *p, uint8_t val,
                    int mmu_idx, uintptr_t ra)
{
    if (unlikely(p->flags & TLB_MMIO)) {
        do_st_mmio_leN(cpu, p->full, val, p->addr, 1, mmu_idx, ra);
    } else if (unlikely(p->flags & TLB_DISCARD_WRITE)) {
        /* nothing */
    } else {
        *(uint8_t *)p->haddr = val;
    }
}

static void do_st_2(CPUState *cpu, MMULookupPageData *p, uint16_t val,
                    int mmu_idx, MemOp memop, uintptr_t ra)
{
    if (unlikely(p->flags & TLB_MMIO)) {
        if ((memop & MO_BSWAP) != MO_LE) {
            val = bswap16(val);
        }
        do_st_mmio_leN(cpu, p->full, val, p->addr, 2, mmu_idx, ra);
    } else if (unlikely(p->flags & TLB_DISCARD_WRITE)) {
        /* nothing */
    } else {
        /* Swap to host endian if necessary, then store. */
        if (memop & MO_BSWAP) {
            val = bswap16(val);
        }
        store_atom_2(cpu, ra, p->haddr, memop, val);
    }
}

static void do_st_4(CPUState *cpu, MMULookupPageData *p, uint32_t val,
                    int mmu_idx, MemOp memop, uintptr_t ra)
{
    if (unlikely(p->flags & TLB_MMIO)) {
        if ((memop & MO_BSWAP) != MO_LE) {
            val = bswap32(val);
        }
        do_st_mmio_leN(cpu, p->full, val, p->addr, 4, mmu_idx, ra);
    } else if (unlikely(p->flags & TLB_DISCARD_WRITE)) {
        /* nothing */
    } else {
        /* Swap to host endian if necessary, then store. */
        if (memop & MO_BSWAP) {
            val = bswap32(val);
        }
        store_atom_4(cpu, ra, p->haddr, memop, val);
    }
}

static void do_st_8(CPUState *cpu, MMULookupPageData *p, uint64_t val,
                    int mmu_idx, MemOp memop, uintptr_t ra)
{
    if (unlikely(p->flags & TLB_MMIO)) {
        if ((memop & MO_BSWAP) != MO_LE) {
            val = bswap64(val);
        }
        do_st_mmio_leN(cpu, p->full, val, p->addr, 8, mmu_idx, ra);
    } else if (unlikely(p->flags & TLB_DISCARD_WRITE)) {
        /* nothing */
    } else {
        /* Swap to host endian if necessary, then store. */
        if (memop & MO_BSWAP) {
            val = bswap64(val);
        }
        store_atom_8(cpu, ra, p->haddr, memop, val);
    }
}

static void do_st1_mmu(CPUState *cpu, vaddr addr, uint8_t val,
                       MemOpIdx oi, uintptr_t ra)
{
    MMULookupLocals l;
    bool crosspage;

    cpu_req_mo(cpu, TCG_MO_LD_ST | TCG_MO_ST_ST);
    crosspage = mmu_lookup(cpu, addr, oi, ra, MMU_DATA_STORE, &l);
    tcg_debug_assert(!crosspage);

    do_st_1(cpu, &l.page[0], val, l.mmu_idx, ra);
}

static void do_st2_mmu(CPUState *cpu, vaddr addr, uint16_t val,
                       MemOpIdx oi, uintptr_t ra)
{
    MMULookupLocals l;
    bool crosspage;
    uint8_t a, b;

    cpu_req_mo(cpu, TCG_MO_LD_ST | TCG_MO_ST_ST);
    crosspage = mmu_lookup(cpu, addr, oi, ra, MMU_DATA_STORE, &l);
    if (likely(!crosspage)) {
        do_st_2(cpu, &l.page[0], val, l.mmu_idx, l.memop, ra);
        return;
    }

    if ((l.memop & MO_BSWAP) == MO_LE) {
        a = val, b = val >> 8;
    } else {
        b = val, a = val >> 8;
    }
    do_st_1(cpu, &l.page[0], a, l.mmu_idx, ra);
    do_st_1(cpu, &l.page[1], b, l.mmu_idx, ra);
}

static void do_st4_mmu(CPUState *cpu, vaddr addr, uint32_t val,
                       MemOpIdx oi, uintptr_t ra)
{
    MMULookupLocals l;
    bool crosspage;

    cpu_req_mo(cpu, TCG_MO_LD_ST | TCG_MO_ST_ST);
    crosspage = mmu_lookup(cpu, addr, oi, ra, MMU_DATA_STORE, &l);
    if (likely(!crosspage)) {
        do_st_4(cpu, &l.page[0], val, l.mmu_idx, l.memop, ra);
        return;
    }

    /* Swap to little endian for simplicity, then store by bytes. */
    if ((l.memop & MO_BSWAP) != MO_LE) {
        val = bswap32(val);
    }
    val = do_st_leN(cpu, &l.page[0], val, l.mmu_idx, l.memop, ra);
    (void) do_st_leN(cpu, &l.page[1], val, l.mmu_idx, l.memop, ra);
}

static void do_st8_mmu(CPUState *cpu, vaddr addr, uint64_t val,
                       MemOpIdx oi, uintptr_t ra)
{
    MMULookupLocals l;
    bool crosspage;

    cpu_req_mo(cpu, TCG_MO_LD_ST | TCG_MO_ST_ST);
    crosspage = mmu_lookup(cpu, addr, oi, ra, MMU_DATA_STORE, &l);
    if (likely(!crosspage)) {
        do_st_8(cpu, &l.page[0], val, l.mmu_idx, l.memop, ra);
        return;
    }

    /* Swap to little endian for simplicity, then store by bytes. */
    if ((l.memop & MO_BSWAP) != MO_LE) {
        val = bswap64(val);
    }
    val = do_st_leN(cpu, &l.page[0], val, l.mmu_idx, l.memop, ra);
    (void) do_st_leN(cpu, &l.page[1], val, l.mmu_idx, l.memop, ra);
}

static void do_st16_mmu(CPUState *cpu, vaddr addr, Int128 val,
                        MemOpIdx oi, uintptr_t ra)
{
    MMULookupLocals l;
    bool crosspage;
    uint64_t a, b;
    int first;

    cpu_req_mo(cpu, TCG_MO_LD_ST | TCG_MO_ST_ST);
    crosspage = mmu_lookup(cpu, addr, oi, ra, MMU_DATA_STORE, &l);
    if (likely(!crosspage)) {
        if (unlikely(l.page[0].flags & TLB_MMIO)) {
            if ((l.memop & MO_BSWAP) != MO_LE) {
                val = bswap128(val);
            }
            do_st16_mmio_leN(cpu, l.page[0].full, val, addr, 16, l.mmu_idx, ra);
        } else if (unlikely(l.page[0].flags & TLB_DISCARD_WRITE)) {
            /* nothing */
        } else {
            /* Swap to host endian if necessary, then store. */
            if (l.memop & MO_BSWAP) {
                val = bswap128(val);
            }
            store_atom_16(cpu, ra, l.page[0].haddr, l.memop, val);
        }
        return;
    }

    first = l.page[0].size;
    if (first == 8) {
        MemOp mop8 = (l.memop & ~(MO_SIZE | MO_BSWAP)) | MO_64;

        if (l.memop & MO_BSWAP) {
            val = bswap128(val);
        }
        if (HOST_BIG_ENDIAN) {
            b = int128_getlo(val), a = int128_gethi(val);
        } else {
            a = int128_getlo(val), b = int128_gethi(val);
        }
        do_st_8(cpu, &l.page[0], a, l.mmu_idx, mop8, ra);
        do_st_8(cpu, &l.page[1], b, l.mmu_idx, mop8, ra);
        return;
    }

    if ((l.memop & MO_BSWAP) != MO_LE) {
        val = bswap128(val);
    }
    if (first < 8) {
        do_st_leN(cpu, &l.page[0], int128_getlo(val), l.mmu_idx, l.memop, ra);
        val = int128_urshift(val, first * 8);
        do_st16_leN(cpu, &l.page[1], val, l.mmu_idx, l.memop, ra);
    } else {
        b = do_st16_leN(cpu, &l.page[0], val, l.mmu_idx, l.memop, ra);
        do_st_leN(cpu, &l.page[1], b, l.mmu_idx, l.memop, ra);
    }
}

#include "ldst_common.c.inc"

/*
 * First set of functions passes in OI and RETADDR.
 * This makes them callable from other helpers.
 */

#define ATOMIC_NAME(X) \
    glue(glue(glue(cpu_atomic_ ## X, SUFFIX), END), _mmu)

#define ATOMIC_MMU_CLEANUP

#include "atomic_common.c.inc"

#define DATA_SIZE 1
#include "atomic_template.h"

#define DATA_SIZE 2
#include "atomic_template.h"

#define DATA_SIZE 4
#include "atomic_template.h"

#define DATA_SIZE 8
#include "atomic_template.h"

#if defined(CONFIG_ATOMIC128) || HAVE_CMPXCHG128
#define DATA_SIZE 16
#include "atomic_template.h"
#endif

/* Code access functions.  */

uint8_t cpu_ldb_code_mmu(CPUArchState *env, vaddr addr,
                         MemOpIdx oi, uintptr_t retaddr)
{
    return do_ld1_mmu(env_cpu(env), addr, oi, retaddr, MMU_INST_FETCH);
}

uint16_t cpu_ldw_code_mmu(CPUArchState *env, vaddr addr,
                          MemOpIdx oi, uintptr_t retaddr)
{
    return do_ld2_mmu(env_cpu(env), addr, oi, retaddr, MMU_INST_FETCH);
}

uint32_t cpu_ldl_code_mmu(CPUArchState *env, vaddr addr,
                          MemOpIdx oi, uintptr_t retaddr)
{
    return do_ld4_mmu(env_cpu(env), addr, oi, retaddr, MMU_INST_FETCH);
}

uint64_t cpu_ldq_code_mmu(CPUArchState *env, vaddr addr,
                          MemOpIdx oi, uintptr_t retaddr)
{
    return do_ld8_mmu(env_cpu(env), addr, oi, retaddr, MMU_INST_FETCH);
}

/*
 * Common pointer_wrap implementations.
 */

/*
 * To be used for strict alignment targets.
 * Because no accesses are unaligned, no accesses wrap either.
 */
vaddr cpu_pointer_wrap_notreached(CPUState *cs, int idx, vaddr res, vaddr base)
{
    g_assert_not_reached();
}

/* To be used for strict 32-bit targets. */
vaddr cpu_pointer_wrap_uint32(CPUState *cs, int idx, vaddr res, vaddr base)
{
    return (uint32_t)res;
}
