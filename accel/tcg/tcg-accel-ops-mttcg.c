/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#ifndef _WIN32
#include <dlfcn.h>
#ifdef __ANDROID__
#include <sys/prctl.h>
#endif
#endif
#include "system/memory.h"
#include "exec/cpu-common.h"
#include "system/tcg.h"
#include "system/replay.h"
#include "exec/icount.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "hw/core/boards.h"
#include "tcg/startup.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"
#ifdef QEMU_EXPERIMENTAL_TB_CACHE_STATS
#include "tb-jmp-cache.h"
#endif
#include "system/cpu-timers.h"

#ifdef CONFIG_DARWIN
#include <pthread/qos.h>
#include "trace.h"
#include "hw/trace-printf.h"
#endif

typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu(cpu, do_nothing, RUN_ON_CPU_NULL);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

/*
 * Ask for a performance core.
 *
 * This is the thread that IS the emulated CPU -- everything the guest does
 * happens here -- but qemu_thread_create() sets no QoS class, so on Apple
 * silicon it is eligible for an efficiency core, at roughly half the
 * throughput. The host app setting its own QoS does not help: that applies to
 * the thread calling qemu_init(), which becomes the iothread, not this one.
 *
 * USER_INTERACTIVE rather than USER_INITIATED because this thread is literally
 * driving what is on screen.
 */
static void mttcg_request_performance_core(void)
{
#ifdef CONFIG_DARWIN
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relpri = 0;

    pthread_get_qos_class_np(pthread_self(), &qos, &relpri);
    if (qos != QOS_CLASS_USER_INTERACTIVE) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
    if (trace_event_get_state_backends(TRACE_MTTCG_VCPU_QOS_LOG)) {
        qos_class_t now = QOS_CLASS_UNSPECIFIED;
        pthread_get_qos_class_np(pthread_self(), &now, &relpri);
        TRACE_PRINTF(trace_mttcg_vcpu_qos_log, "[qos] vcpu thread: was %d, now %d "
                "(USER_INTERACTIVE=%d)\n",
                (int)qos, (int)now, (int)QOS_CLASS_USER_INTERACTIVE);
    }
#endif
}

/*
 * IT_VCPU_ACCT: split this thread's wall time three ways.
 *
 * "run" is time inside the engine with the guest executing, "halt" is time the
 * guest spent halted (WFI) waiting for an interrupt, and "io" is everything
 * else -- taking the BQL back, servicing run_on_cpu work, the main loop. If a
 * slow emulator is not spending its time in "run", the engine is not the
 * bottleneck and optimizing it is wasted effort.
 *
 * cpu_exec() returns EXCP_HALTED without executing anything when the guest is
 * halted, which is what lets the two be told apart from out here.
 */
extern bool it_acct_on;
extern bool it_bql_acct_on;
extern bool it_profile_on;
extern uint64_t it_acct_insns, it_acct_blocks, it_acct_irqs, it_acct_exits;
extern uint64_t it_acct_lookups, it_acct_jc_misses, it_acct_jc_empty, it_acct_jc_other_pc;
extern uint64_t it_acct_flagbit[3][32];
extern uint64_t it_acct_pcrel_invals, it_acct_jc_flushes;
extern uint64_t it_acct_vcpu_bql_n, it_acct_vcpu_bql_ns, it_acct_vcpu_bql_slow;
extern struct it_acct_mr { void *mr; uint64_t n, ns; char *name; uint64_t hold_n, hold_ns, hold_long_n, hold_max; } it_acct_mrs[256];
extern uint64_t it_acct_wait_hist[5][2], it_acct_hold_hist[5][2];
extern uint64_t it_acct_ctxsw, it_acct_fills[32][2];
extern uint64_t it_acct_tlbi_is[4];
extern uint64_t it_acct_el0_sync, it_acct_el0_used, it_acct_tlbiasid, it_acct_pflush, it_acct_rflush,
    it_acct_reset_dirty;
extern struct it_acct_holder { const char *file; int line; uint64_t n, ns, long_n, long_ns; } it_acct_holders[64];
extern struct it_acct_cb { void *fn; uint64_t n, ns, long_n, long_ns, max; } it_acct_cbs[64];
void it_acct_cb_enable(void);

static int64_t acct_run_ns, acct_halt_ns, acct_io_ns, acct_bql_ns;
static int64_t acct_io_cpu_ns, acct_io_cpu_mark;
static int64_t acct_t0, acct_next_dump, acct_interval_ns, acct_io_mark;
static uint64_t acct_halts;
static int64_t acct_run_since;          /* start of the exec in progress, 0 outside one */
static clockid_t acct_cpu_clock;
static bool acct_cpu_clock_ok;
static uint64_t acct_why_halted, acct_why_stopped, acct_why_other, acct_long;
static bool acct_halted;

/*
 * Wall time in the io window says the vCPU is not executing; it does NOT say
 * whether the thread is asleep or busy. Those need opposite fixes -- asleep
 * means something is failing to wake it, busy means queued vCPU work (TLB
 * flushes arrive as async_run_on_cpu items and run right here) is eating the
 * budget. The thread CPU clock tells them apart.
 */
static int64_t thread_cpu_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) {
        return 0;
    }
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * The accounting follows the board's first CPU only. Boards with a second core (the A4's IOP,
 * running Apple's NAND firmware) start its thread after the first, and it used to take over the
 * CPU clock and add its own execs to "run": a Jetpack profile showed the IOP's 0.08 ms a frame
 * as the vCPU's CPU time, and more run than wall time while an app loaded.
 */
static int acct_vcpu_tid;

static void mttcg_acct_init(CPUState *cpu)
{
    const char *v = getenv("IT_VCPU_ACCT");
    const char *bql = getenv("LT_BQL_PROF");
    bool bql_only = bql && strcmp(bql, "off") && strcmp(bql, "0");

    if (cpu->cpu_index != 0) {
        return;
    }
    acct_vcpu_tid = qemu_get_thread_id();
#ifdef __ANDROID__
    {
        /* Hardware memory tagging checks every heap access the translator and its TLBs make
         * (GrapheneOS can enable it for apps); hitch reports carry this line. */
        int ctrl = prctl(56 /* PR_GET_TAGGED_ADDR_CTRL */, 0, 0, 0, 0);
        fprintf(stderr, "[vcpu] memory tagging: %s\n", ctrl < 0 ? "unavailable" :
                (ctrl & (1 << 1)) && (ctrl & (1 << 2)) ? "asymmetric" : (ctrl & (1 << 1)) ? "sync" :
                (ctrl & (1 << 2)) ? "async" : "off");
    }
#endif
#if !defined(CONFIG_DARWIN) && !defined(_WIN32) /* no pthread_getcpuclockid */
    acct_cpu_clock_ok = !pthread_getcpuclockid(pthread_self(), &acct_cpu_clock);
#endif
    if (!v && !bql_only) {
        return;
    }
    it_acct_on = v != NULL;
    qatomic_set(&it_bql_acct_on, true);
    acct_interval_ns = (int64_t)(v && atof(v) > 0 ? atof(v) : 5.0) *
                       1000000000LL;
    acct_t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    acct_next_dump = acct_t0 + acct_interval_ns;
    it_acct_cb_enable();
    it_acct_excl_on = true;
    fprintf(stderr, "[acct-mode] full=%s bql=on begin-ns %" PRId64
            " interval-ns %" PRId64 "\n", it_acct_on ? "on" : "off",
            acct_t0, acct_interval_ns);
}

/*
 * Cumulative vCPU time for qemu-ios-ui.c's per-frame trace (IT_FRAME_TRACE),
 * so a slow frame can be put down to guest work, guest idle, the BQL or the
 * host taking the CPU away: v[0] run (in cpu_exec, including GL calls and
 * the BQL waits of MMIO inside it), v[1] outside cpu_exec (halted, idle or
 * running queued work, including v[2]), v[2] BQL waits outside cpu_exec,
 * v[3] BQL waits inside it, v[4] vCPU thread CPU time, v[5] guest
 * instructions. Read from another thread without locks: diagnostics only, a
 * figure can be one exec stale. Timing also runs during manual recording or
 * LT_BQL_PROF; instruction counts require IT_VCPU_ACCT.
 */
void it_acct_frame_snapshot(int64_t v[6])
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t since = qatomic_read(&acct_run_since), mark = qatomic_read(&acct_io_mark);
    struct timespec ts;

    v[0] = qatomic_read(&acct_run_ns) + (since ? now - since : 0);
    v[1] = qatomic_read(&acct_halt_ns) + qatomic_read(&acct_io_ns) +
           (!since && mark ? now - mark : 0);
    v[2] = qatomic_read(&acct_bql_ns);
    v[3] = (int64_t)qatomic_read(&it_acct_vcpu_bql_ns);
    v[4] = acct_cpu_clock_ok && !clock_gettime(acct_cpu_clock, &ts) ?
           ts.tv_sec * 1000000000LL + ts.tv_nsec : 0;
    v[5] = (int64_t)qatomic_read(&it_acct_insns);
}

/*
 * Where the host runs the first vCPU, for the per-frame profile: v[0] cumulative ns it waited
 * runnable for a host core (schedstat; preemption and contention), v[1] the core it ran on last,
 * v[2] that core's clock in kHz. -1 where the host has no /proc (macOS, Windows).
 */
void it_acct_vcpu_sched(int64_t v[3])
{
    v[0] = v[1] = v[2] = -1;
#ifdef __linux__
    static int schedstat = -1, stat = -1, freq[32];
    static bool opened;
    char buf[512], *p;
    ssize_t n;
    unsigned long long run, wait;

    if (!opened && acct_vcpu_tid) {
        opened = true;
        snprintf(buf, sizeof(buf), "/proc/self/task/%d/schedstat", acct_vcpu_tid);
        schedstat = open(buf, O_RDONLY | O_CLOEXEC);
        snprintf(buf, sizeof(buf), "/proc/self/task/%d/stat", acct_vcpu_tid);
        stat = open(buf, O_RDONLY | O_CLOEXEC);
        for (int i = 0; i < ARRAY_SIZE(freq); i++) {
            snprintf(buf, sizeof(buf), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
            freq[i] = open(buf, O_RDONLY | O_CLOEXEC);
        }
    }
    if (schedstat >= 0 && (n = pread(schedstat, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        if (sscanf(buf, "%llu %llu", &run, &wait) == 2) {
            v[0] = wait;
        }
    }
    /* Field 39, "processor", counted after the parenthesized name, which may hold spaces. */
    if (stat >= 0 && (n = pread(stat, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        p = strrchr(buf, ')');
        for (int field = 2; p && field < 39; field++) {
            p = strchr(p + 1, ' ');
        }
        if (p) {
            v[1] = atoi(p + 1);
        }
    }
    if (v[1] >= 0 && v[1] < ARRAY_SIZE(freq) && freq[v[1]] >= 0 &&
        (n = pread(freq[v[1]], buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        v[2] = atoll(buf);
    }
#endif
}

static void mttcg_acct_dump(int64_t now)
{
    double wall = (now - acct_t0) / 1e9;

    if (wall <= 0) {
        return;
    }
    fprintf(stderr, "[acct] %6.1fs  run %5.1f%%  halt %5.1f%%  io %5.1f%% "
            "(bql %4.1f%% cpu %4.1f%%)  |  "
            "%.2f Minsn/s  %.0f blk/s (%.1f insn/blk)  %.0f irq/s  "
            "%.0f halt/s  %.0f exit/s  |  long %lu (halted %lu stopped %lu other %lu)"
            "  |  lookups %" PRIu64 " jc-miss %" PRIu64 " (empty %" PRIu64
            " other-pc %" PRIu64 ")  pcrel-inval %" PRIu64 " jc-flush %" PRIu64 "\n",
            wall,
            100.0 * acct_run_ns / (now - acct_t0),
            100.0 * acct_halt_ns / (now - acct_t0),
            100.0 * acct_io_ns / (now - acct_t0),
            100.0 * acct_bql_ns / (now - acct_t0),
            100.0 * acct_io_cpu_ns / (now - acct_t0),
            it_acct_insns / wall / 1e6,
            it_acct_blocks / wall,
            it_acct_blocks ? (double)it_acct_insns / it_acct_blocks : 0.0,
            it_acct_irqs / wall,
            acct_halts / wall,
            it_acct_exits / wall,
            (unsigned long)acct_long, (unsigned long)acct_why_halted,
            (unsigned long)acct_why_stopped, (unsigned long)acct_why_other,
            it_acct_lookups, it_acct_jc_misses, it_acct_jc_empty,
            it_acct_jc_other_pc, it_acct_pcrel_invals, it_acct_jc_flushes);
    fprintf(stderr, "[acct]   bounds begin-ns %" PRId64 " end-ns %" PRId64
            " full=%s\n", acct_t0, now, it_acct_on ? "on" : "off");
    fprintf(stderr, "[acct]   vcpu-bql n %" PRIu64 " wait-ns %" PRIu64 " slow %" PRIu64 "\n",
            it_acct_vcpu_bql_n, it_acct_vcpu_bql_ns, it_acct_vcpu_bql_slow);
    for (int i = 0; i < 64 && it_acct_cbs[i].fn; i++) {
        struct it_acct_cb *c = &it_acct_cbs[i];
#ifdef _WIN32
        fprintf(stderr, "[acct]   main-cb ?+0x%" PRIxPTR " - n %" PRIu64 " ns %" PRIu64
                " long %" PRIu64 " long-ns %" PRIu64 " max %" PRIu64 "\n",
                (uintptr_t)c->fn, c->n, c->ns, c->long_n, c->long_ns, c->max);
#else
        Dl_info di = { 0 };

        /* Symbolize offline from the library's offset (static functions have no dynamic name). */
        dladdr(c->fn, &di);
        fprintf(stderr, "[acct]   main-cb %s+0x%" PRIxPTR " %s n %" PRIu64 " ns %" PRIu64
                " long %" PRIu64 " long-ns %" PRIu64 " max %" PRIu64 "\n",
                di.dli_fname ? strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname : "?",
                (uintptr_t)c->fn - (uintptr_t)di.dli_fbase, di.dli_sname ? di.dli_sname : "-",
                c->n, c->ns, c->long_n, c->long_ns, c->max);
#endif
    }
    for (int i = 0; i < ARRAY_SIZE(it_acct_excl) && it_acct_excl[i].fn; i++) {
#ifndef _WIN32
        struct it_acct_excl *e = &it_acct_excl[i];
        Dl_info di = { 0 };

        dladdr(e->fn, &di);
        fprintf(stderr, "[acct]   exclusive %s+0x%" PRIxPTR " n %" PRIu64 " wait-ns %" PRIu64 " work-ns %" PRIu64
                " max-wait %" PRIu64 "\n", di.dli_sname ? di.dli_sname : "lib",
                (uintptr_t)e->fn - (uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase), e->n, e->wait_ns,
                e->work_ns, e->max_wait);
#endif
    }
    fprintf(stderr, "[acct]   tlbi-is all %" PRIu64 " asid %" PRIu64 " mva %" PRIu64 " mvaa %" PRIu64 "\n",
            it_acct_tlbi_is[0], it_acct_tlbi_is[1], it_acct_tlbi_is[2], it_acct_tlbi_is[3]);
    fprintf(stderr, "[acct]   ctxsw %" PRIu64 " el0-sync %" PRIu64 " el0-used %" PRIu64 " tlbiasid %" PRIu64
            " pflush %" PRIu64 " rflush %" PRIu64 " reset-dirty %" PRIu64 " tlb-fills", it_acct_ctxsw, it_acct_el0_sync,
            it_acct_el0_used, it_acct_tlbiasid, it_acct_pflush, it_acct_rflush, it_acct_reset_dirty);
    for (int i = 0; i < 32; i++) {
        if (it_acct_fills[i][0] || it_acct_fills[i][1]) {
            fprintf(stderr, " idx%d %" PRIu64 "/%" PRIu64, i, it_acct_fills[i][0], it_acct_fills[i][1]);
        }
    }
    fprintf(stderr, "\n");
    for (int k = 0; k < 2; k++) {
        uint64_t (*hst)[2] = k ? it_acct_hold_hist : it_acct_wait_hist;
        fprintf(stderr, "[acct]   bql-%s-hist", k ? "hold" : "wait");
        for (int b = 0; b < 5; b++) {
            fprintf(stderr, " %" PRIu64 "/%" PRIu64, hst[b][0], hst[b][1]);
        }
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < 64 && it_acct_holders[i].file; i++) {
        fprintf(stderr, "[acct]   bql-holder %s:%d n %" PRIu64 " ns %" PRIu64
                " long %" PRIu64 " long-ns %" PRIu64 "\n",
                it_acct_holders[i].file, it_acct_holders[i].line, it_acct_holders[i].n,
                it_acct_holders[i].ns, it_acct_holders[i].long_n, it_acct_holders[i].long_ns);
    }
    for (int i = 0; i < 256 && it_acct_mrs[i].mr; i++) {
        fprintf(stderr, "[acct]   bql-by-mr %s n %" PRIu64 " wait-ns %" PRIu64 " hold-n %" PRIu64 " hold-ns %" PRIu64
                " hold-long %" PRIu64 " hold-max %" PRIu64 "\n",
                it_acct_mrs[i].name, it_acct_mrs[i].n, it_acct_mrs[i].ns, it_acct_mrs[i].hold_n,
                it_acct_mrs[i].hold_ns, it_acct_mrs[i].hold_long_n, it_acct_mrs[i].hold_max);
    }
    for (int k = 0; k < 3; k++) {
        for (int b = 0; b < 32; b++) {
            if (it_acct_flagbit[k][b]) {
                fprintf(stderr, "[acct]   same-pc miss: %s bit %d x%" PRIu64 "\n",
                        k == 0 ? "cs_base" : k == 1 ? "flags" : "cflags", b,
                        it_acct_flagbit[k][b]);
            }
        }
    }
    {
        extern struct it_mmio_hist { void *mr; uint32_t off; uint64_t n; char *name; } it_mmio_hist[4096];
        extern int it_mmio_hist_on;

        for (int i = 0; it_mmio_hist_on > 0 && i < 4096; i++) {
            struct it_mmio_hist *e = &it_mmio_hist[i];
            if (qatomic_load_acquire(&e->mr) && e->n >= 64) {
                fprintf(stderr, "[acct]   mmio %s+0x%x %c n %" PRIu64 "\n", e->name, e->off & ~1u,
                        e->off & 1 ? 'w' : 'r', e->n);
            }
        }
    }
    fprintf(stderr, "[acct]   end-ns %" PRId64 "\n", now);
}

/*
 * The guest's clock waits for a vCPU the host isn't running (MachineClass clock_waits_for_vcpus).
 *
 * A board's cores never stop while its timebase runs. A vCPU thread stops whenever the host deschedules it or it
 * waits for the BQL or for the other vCPUs (a broadcast TLB flush is exclusive work), and the VM clock, which is host
 * time, runs on. A core spinning on a lock against the timebase then counts the holder's stall as time: iOS 10's
 * hw_lock_bit (14G61, 800da1ee) panics "hw_lock_bit(): timed out" after 0xc00000 ticks, 0.52 s at 24 MHz.
 *
 * Every STALL_TICK_US this thread looks for a vCPU that is neither asleep idle nor has reached a TB boundary since the
 * last look, and kicks each awake one, so that one the host runs reaches a TB boundary within a TB; while there is a
 * stalled one, the VM clock runs at 1/16 (cpu_clock_set_slow), and afterwards at 1.25 times host time until it has
 * caught up, so the guest's date doesn't fall behind the host's; a holder that runs is nowhere near a lock timeout,
 * and a stall during the catch-up slows the clock again. It looks every STALL_IDLE_TICK_US while every vCPU is idle
 * and the clock is at host rate, as none can stall then (one that wakes counts as seen).
 */
#define STALL_TICK_US 2000
#define STALL_IDLE_TICK_US 20000

/* One look at the vCPUs: whether one stalled since the last; *awake, whether one is awake now. */
static bool mttcg_stall_check(bool *awake)
{
    CPUState *cpu;
    bool stalled = false;

    *awake = false;
    CPU_FOREACH(cpu) {
        bool seen = qatomic_xchg(&cpu->exec_seen, false);

        if (!qatomic_read(&cpu->created) || qatomic_read(&cpu->idle)) {
            continue;
        }
        *awake = true;
        stalled |= !seen;
        /* cpu_exit without its exit_request: out of the TB chain into cpu_handle_interrupt, not out of cpu_exec */
        qatomic_set(&cpu->neg.icount_decr.u16.high, -1);
    }
    return stalled;
}

/*
 * A look, and the VM clock's rate after it; *awake as mttcg_stall_check's. Returns whether the clock is off host rate.
 * While it catches up, the main loop's timer deadlines, which assume host rate, come due early: each look wakes it.
 */
static bool mttcg_stall_look(bool *awake)
{
    bool stalled, off_rate;

    WITH_RCU_READ_LOCK_GUARD() {
        stalled = mttcg_stall_check(awake);
    }
    off_rate = cpu_clock_set_slow(stalled);
    if (off_rate && !stalled) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
    return off_rate;
}

static void *mttcg_stall_thread(void *arg)
{
    bool awake = true, off_rate = false;

    rcu_register_thread();
    mttcg_request_performance_core();   /* the vCPUs' QoS: a look the host delays is time the guest sees */
    for (;;) {
        g_usleep(awake || off_rate ? STALL_TICK_US : STALL_IDLE_TICK_US);
        off_rate = mttcg_stall_look(&awake);
    }
    return NULL;
}

static void mttcg_stall_start(void)
{
    static QemuThread thread;
    static bool started;

    if (!started && MACHINE_GET_CLASS(current_machine)->clock_waits_for_vcpus) {
        started = true;
        qemu_thread_create(&thread, "vCPU stall check", mttcg_stall_thread, NULL, QEMU_THREAD_DETACHED);
    }
}

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    mttcg_request_performance_core();
    mttcg_acct_init(cpu);
    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

    bql_lock();
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    current_cpu = cpu;
    cpu_thread_signal_created(cpu);
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    do {
        /* Keep one timing decision across exec and relock, including stop races. */
        bool timing = cpu->cpu_index == 0 && (qatomic_read(&it_bql_acct_on) ||
                                              qatomic_read(&it_profile_on));
        if (!timing) {
            qatomic_set(&acct_io_mark, 0);
        }
        bool idle = cpu_thread_is_idle(cpu);

        if (idle) {
            qatomic_set(&cpu->idle, true);
        }
        qemu_process_cpu_events(cpu);
        if (idle) {
            qatomic_set(&cpu->exec_seen, true);
            qatomic_set(&cpu->idle, false);
        }

        if (timing) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (acct_io_mark) {
                if (acct_halted) {
                    acct_halt_ns += now - acct_io_mark;
                } else {
                    acct_io_ns += now - acct_io_mark;
                    acct_io_cpu_ns += thread_cpu_ns() - acct_io_cpu_mark;
                    /*
                     * Only long waits matter, and the reason has to be sampled
                     * right after waking or it has already been cleared.
                     */
                    if (now - acct_io_mark > 2000000) {
                        acct_long++;
                        if (cpu->halted) {
                            acct_why_halted++;
                        } else if (!runstate_is_running()) {
                            acct_why_stopped++;
                        } else {
                            acct_why_other++;
                        }
                    }
                }
                acct_io_mark = 0;
            }
            if (qatomic_read(&it_bql_acct_on) && now >= acct_next_dump) {
                acct_next_dump = now + acct_interval_ns;
                mttcg_acct_dump(now);
#ifdef QEMU_EXPERIMENTAL_TB_CACHE_STATS
                if (it_acct_on) {
                    tb_cache_clear_dump(stderr, (now - acct_t0) / 1e9,
                                        cpu->cpu_index, TB_JMP_CACHE_SIZE,
                                        &cpu->tb_jmp_cache->clear_stats);
                }
#endif
            }
        }

        if (cpu_can_run(cpu)) {
            int r;
            int64_t a = 0, b;
            bql_unlock();
            if (timing) {
                a = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                qatomic_set(&acct_run_since, a);
            }
            r = tcg_cpu_exec(cpu);
            if (timing) {
                b = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                qatomic_set(&acct_run_since, 0);
                /*
                 * cpu_exec() RETURNS on a halt, cheaply -- the actual sleep is
                 * inside qemu_process_cpu_events() at the top of the loop, so the wait has to be
                 * attributed by what the preceding exec returned, or halt time
                 * hides inside "io" and the split says nothing.
                 */
                acct_run_ns += b - a;
                acct_halted = (r == EXCP_HALTED);
                acct_halts += acct_halted;
                acct_io_mark = b;   /* closed after qemu_process_cpu_events */
                acct_io_cpu_mark = thread_cpu_ns();
            }
            bql_lock();
            /*
             * "io" is two very different things -- waiting for the BQL (the
             * iothread is holding it, e.g. across an LCD conversion) and
             * waiting in the main loop itself. Only the first is contention,
             * and only the first is fixable here, so time it separately.
             */
            if (timing) {
                acct_bql_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - b;
            }
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                bql_unlock();
                cpu_exec_step_atomic(cpu);
                bql_lock();
            default:
                /* Ignore everything else? */
                break;
            }
        }
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpu_destroy(cpu);
    bql_unlock();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();
    return NULL;
}

void mttcg_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    mttcg_stall_start();
    /*
     * CF_PARALLEL makes guest atomics host atomics and guest barriers host fences, and it turns
     * off the per-site indirect branch cache, whose slots the executing vCPU writes. A CPU its
     * board marks tcg_unshared needs none of that even beside another vCPU: the A4 and S5L8920
     * boards' application processor runs alongside an IOP core with its own code and no
     * exclusives (an arm946), the two talking through mailbox MMIO under the BQL.
     * LT_TCG_PARALLEL=on translates every CPU parallel again (A/B).
     */
    static int force_parallel = -1;
    if (force_parallel < 0) {
        const char *v = getenv("LT_TCG_PARALLEL");
        force_parallel = v && !strcmp(v, "on");
    }
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1 &&
                             (force_parallel || !cpu->tcg_unshared));

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}
