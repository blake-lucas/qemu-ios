/*
 * LT_SAMPLE=HZ: a CPU-time sampler of the vCPU threads, for hosts where perf can't run (no root, or the guest
 * context is wanted with each sample).
 *
 * Each vCPU thread gets a timer on its own CPU-time clock that signals that thread; the handler stores the host PC
 * and the guest's PC, CPSR, TTBR0 (the process) and TPIDRURO (the thread). Sampling runs while a profile capture
 * does (qemu_ios_profile_start/stop), and the stop writes CAPTURE.samples: the samples, the host mappings to
 * symbolize them, and for each sample whether its host PC was in translated code.
 */
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "cpu.h"
#include "tcg/tcg.h"
#include "exec/translation-block.h"
#include "hw/arm/it-sampler.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/syscall.h>

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

typedef struct {
    uint64_t hpc;
    uint32_t gpc, cpsr, ttbr0, tpid;
} ItSample;

#define IT_SAMPLER_THREADS 8
#define IT_SAMPLER_CAP (1u << 18)

typedef struct {
    CPUState *cpu;
    int tid;
    bool ready;
    timer_t timer;
    ItSample *buf;
    uint32_t n, lost;
} ItSamplerThread;

static ItSamplerThread it_st[IT_SAMPLER_THREADS];
static int it_st_n;
static long it_sampler_period_ns;
static bool it_sampler_on;
static bool it_sampler_handler_set;

static void it_sampler_signal(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    int tid = syscall(SYS_gettid);
    int saved = errno;

    for (int i = 0; i < IT_SAMPLER_THREADS; i++) {
        ItSamplerThread *t = &it_st[i];

        if (t->tid != tid || !t->ready) {
            continue;
        }
        if (!qatomic_read(&it_sampler_on)) {
            break;
        }
        if (t->n >= IT_SAMPLER_CAP) {
            t->lost++;
            break;
        }
        ItSample *s = &t->buf[t->n];
        CPUARMState *env = cpu_env(t->cpu);

#ifdef __x86_64__
        s->hpc = uc->uc_mcontext.gregs[REG_RIP];
#else
        s->hpc = uc->uc_mcontext.pc;
#endif
        s->gpc = env->regs[15];
        s->cpsr = env->uncached_cpsr;
        s->ttbr0 = env->cp15.ttbr0_ns ? env->cp15.ttbr0_ns : env->cp15.ttbr0_s;
        s->tpid = env->cp15.tpidruro_ns ? env->cp15.tpidruro_ns : env->cp15.tpidruro_s;
        t->n++;
        break;
    }
    errno = saved;
}

static void it_sampler_arm(ItSamplerThread *t, bool on)
{
    struct itimerspec its = { 0 };

    if (on) {
        its.it_value.tv_nsec = it_sampler_period_ns;
        its.it_interval.tv_nsec = it_sampler_period_ns;
    }
    timer_settime(t->timer, 0, &its, NULL);
}

/* On the vCPU's own thread: its timer, and SIGPROF let through (QEMU blocks signals on its threads). */
static void it_sampler_register(CPUState *cpu, run_on_cpu_data data)
{
    int i = qatomic_fetch_inc(&it_st_n);
    ItSamplerThread *t;
    struct sigevent sev = { 0 };
    clockid_t clock;
    sigset_t set;

    if (i >= IT_SAMPLER_THREADS) {
        return;
    }
    t = &it_st[i];
    t->cpu = cpu;
    t->tid = syscall(SYS_gettid);
    t->buf = g_new0(ItSample, IT_SAMPLER_CAP);
    if (pthread_getcpuclockid(pthread_self(), &clock)) {
        return;
    }
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev.sigev_notify_thread_id = t->tid;
    if (timer_create(clock, &sev, &t->timer)) {
        fprintf(stderr, "[sample] cpu%d: timer_create: %s\n", cpu->cpu_index, strerror(errno));
        return;
    }
    sigemptyset(&set);
    sigaddset(&set, SIGPROF);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    qatomic_set(&t->ready, true);
    if (qatomic_read(&it_sampler_on)) {
        it_sampler_arm(t, true);
    }
}

void it_sampler_start(void)
{
    const char *v = getenv("LT_SAMPLE");
    int hz = v ? atoi(v) : 0;
    CPUState *cpu;

    if (hz <= 0) {
        return;
    }
    it_sampler_period_ns = 1000000000L / MIN(hz, 20000);
    if (!it_sampler_handler_set) {
        struct sigaction sa = { 0 };

        sa.sa_sigaction = it_sampler_signal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, NULL);
        it_sampler_handler_set = true;
        CPU_FOREACH(cpu) {
            async_run_on_cpu(cpu, it_sampler_register, RUN_ON_CPU_NULL);
        }
    }
    for (int i = 0; i < MIN(qatomic_read(&it_st_n), IT_SAMPLER_THREADS); i++) {
        it_st[i].n = it_st[i].lost = 0;
    }
    qatomic_set(&it_sampler_on, true);
    for (int i = 0; i < MIN(qatomic_read(&it_st_n), IT_SAMPLER_THREADS); i++) {
        if (qatomic_read(&it_st[i].ready)) {
            it_sampler_arm(&it_st[i], true);
        }
    }
}

void it_sampler_stop(const char *capture)
{
    g_autofree char *path = NULL;
    Dl_info dl;
    FILE *f, *maps;
    char line[512];

    if (!qatomic_read(&it_sampler_on)) {
        return;
    }
    qatomic_set(&it_sampler_on, false);
    for (int i = 0; i < MIN(qatomic_read(&it_st_n), IT_SAMPLER_THREADS); i++) {
        if (qatomic_read(&it_st[i].ready)) {
            it_sampler_arm(&it_st[i], false);
        }
    }
    path = g_strdup_printf("%s.samples", capture);
    f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "# it-sampler period-ns %ld\n", it_sampler_period_ns);
    if (dladdr((void *)it_sampler_stop, &dl)) {
        fprintf(f, "# engine %s base %p\n", dl.dli_fname, dl.dli_fbase);
    }
    maps = fopen("/proc/self/maps", "r");
    while (maps && fgets(line, sizeof(line), maps)) {
        if (strstr(line, " r-xp ") || strstr(line, " rwxp ") || strstr(line, " --xp ")) {
            fprintf(f, "M %s", line);
        }
    }
    if (maps) {
        fclose(maps);
    }
    for (int i = 0; i < MIN(qatomic_read(&it_st_n), IT_SAMPLER_THREADS); i++) {
        ItSamplerThread *t = &it_st[i];

        if (!t->ready) {
            continue;
        }
        fprintf(f, "T %d tid %d cpu %d samples %u lost %u\n", i, t->tid, t->cpu->cpu_index, t->n, t->lost);
        for (uint32_t k = 0; k < t->n; k++) {
            ItSample *s = &t->buf[k];
            TranslationBlock *tb = tcg_tb_lookup(s->hpc);

            fprintf(f, "S %d %" PRIx64 " %x %x %x %x %d\n", t->cpu->cpu_index, s->hpc, s->gpc, s->cpsr, s->ttbr0,
                    s->tpid, tb ? 1 : 0);
        }
    }
    fclose(f);
}

#else

void it_sampler_start(void)
{
}

void it_sampler_stop(const char *capture)
{
}

#endif
