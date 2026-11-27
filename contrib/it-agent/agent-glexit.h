/*
 * The GL bridge's process exits, watched for the host (included by it_agent.c).
 *
 * A guest process that dies never deletes its GL contexts, so the host kept their textures and buffers charged
 * against its GL memory cap until the device stopped. The GL front end names its process when it makes a sharegroup
 * (pid and the kernel's start time: contrib/it-gles/mbxshim.c, gles_owner). Every two seconds this asks the host for
 * the owners it holds (GLES_OP_OWNERS) when they changed, looks each one up in the kernel, and reports one that has
 * exited (DELETE_SHAREGROUP's owner form), so the host frees its objects.
 *
 * Freeing a live process's objects is worse than the leak, so an owner is reported only when the kernel says so: no
 * process with its pid, a zombie, or a process with its pid that started at another time (a later process). Anything
 * else (a lookup that fails, a layout that does not check out) keeps it. iOS's memory compressor and app suspension
 * do not end a process, so a suspended app keeps its objects. The host's own switch, LT_GLES_PROC_EXIT=off, makes it
 * keep them anyway, for an A/B run.
 */
#include <sys/sysctl.h>

#define GX_QC_GLES 0x140
#define GX_OP_DELETE_SHAREGROUP 0x1005    /* include/hw/arm/guest-services/gles.h */
#define GX_OP_OWNERS 0x100c
#define GX_OWNERS_MAX 64                  /* GLES_OWNERS_MAX */
#define GX_MAX 256                        /* the host's sharegroup limit (GLES_MAX_CONTEXTS) */
#define GX_PERIOD 8                       /* agent ticks (250 ms) between looks */
#define GX_BACKOFF 240                    /* between looks after three refusals in a row (an older engine) */

typedef struct {
    unsigned pid, sec, usec;              /* sec 0: usec is a token (GLES_OWNER_NOT_START), not a start time */
} gx_owner;

static gx_owner gx_list[GX_MAX];
static unsigned char gx_reported[GX_MAX];
static unsigned gx_n, gx_gen, gx_failures;
static gx_owner gx_page[GX_OWNERS_MAX];

static long long gx_call(unsigned slot, unsigned argc, const unsigned *args)
{
    volatile qemu_call_t q;
    unsigned i;

    for (i = 0; i < sizeof(q); i++) ((volatile unsigned char *)&q)[i] = 0;
    q.call_number = GX_QC_GLES;
    q.gles.slot = slot;
    q.gles.argc = argc;
    for (i = 0; i < argc && i < QC_GLES_INLINE_ARGS; i++) q.gles.args[i] = args[i];
    qemu_call_trap(&q);
    return q.retval;
}

/* The host's owner list, when it changed since the last look. 0 when the list is current, -1 on a refusal: an error,
 * or generation 0, which the host never answers (an engine without the list answers an unknown call with 0). */
static int gx_fetch(void)
{
    static gx_owner next[GX_MAX];
    unsigned total = 0, got = 0, gen = 0, i, j;

    while (1) {
        unsigned args[4];
        long long r;

        /* the host writes into this page: touch it so it is mapped and writable */
        memset(gx_page, 0, sizeof(gx_page));
        args[0] = (unsigned)(unsigned long)gx_page;
        args[1] = GX_OWNERS_MAX;
        args[2] = got;
        args[3] = got ? 0 : gx_gen;       /* a later page is always written */
        r = gx_call(GX_OP_OWNERS, 4, args);
        if (r < 0) return -1;
        if (!got) {
            gen = (unsigned)(r >> 32);
            total = (unsigned)r;
            if (!gen) return -1;
            if (gen == gx_gen) return 0;
            if (total > GX_MAX) total = GX_MAX;
        } else if ((unsigned)(r >> 32) != gen) {
            return 0;                     /* changed while paging: look again next time */
        }
        for (i = 0; i < GX_OWNERS_MAX && got < total; i++) next[got++] = gx_page[i];
        if (got >= total) break;
    }
    /* an owner already reported (and kept: LT_GLES_PROC_EXIT=off) is not reported again */
    for (i = 0; i < total; i++) {
        unsigned char reported = 0;

        for (j = 0; j < gx_n; j++) {
            if (gx_reported[j] && gx_list[j].pid == next[i].pid && gx_list[j].sec == next[i].sec &&
                gx_list[j].usec == next[i].usec) {
                reported = 1;
                break;
            }
        }
        gx_reported[i] = reported;
    }
    for (i = 0; i < total; i++) gx_list[i] = next[i];
    gx_n = total;
    gx_gen = gen;
    return 0;
}

/* 1 only when the kernel says the owner's process has exited. */
static int gx_exited(const gx_owner *o)
{
    /* kinfo_proc's extern_proc (32-bit): p_starttime at 0, p_stat at 20, p_pid at 24 */
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)o->pid };
    unsigned kp[256];
    size_t len = sizeof(kp);

    if (!o->pid || o->pid > 0x7fffffff) return 0;
    if (sysctl(mib, 4, kp, &len, 0, 0)) {
        /* no answer: the pid alone, as kill(2) sees it */
        return kill((pid_t)o->pid, 0) && errno == ESRCH;
    }
    if (!len) return 1;                   /* no process has the pid */
    if (len < 28 || kp[6] != o->pid) return 0;
    if (((unsigned char *)kp)[20] == 5) return 1;     /* SZOMB */
    if (o->sec && (kp[0] != o->sec || kp[1] != o->usec)) return 1;   /* a later process with the pid */
    return 0;
}

static void glexit_tick(void)
{
    static unsigned wait;               /* a countdown: no division (the toolchain has no __umodsi3) */
    unsigned i;

    if (wait) {
        wait--;
        return;
    }
    wait = GX_PERIOD - 1;
    if (gx_fetch() < 0) {
        /* an older engine has no owner list, and a host that refuses GL for a while refuses this too: ask once a
         * minute after three refusals in a row */
        if (++gx_failures == 3) w("it_agent: the host lists no GL owners; asking once a minute\n");
        if (gx_failures >= 3) wait = GX_BACKOFF - 1;
        return;
    }
    gx_failures = 0;
    for (i = 0; i < gx_n; i++) {
        if (gx_reported[i] || !gx_exited(&gx_list[i])) continue;
        unsigned args[3] = { gx_list[i].pid, gx_list[i].sec, gx_list[i].usec };
        /* a refused report is made again at the next look */
        if (gx_call(GX_OP_DELETE_SHAREGROUP, 3, args) >= 0) gx_reported[i] = 1;
    }
}
