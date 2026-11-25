/* SPDX-License-Identifier: GPL-2.0-or-later */

/* A bounded alternative to immediately sleeping on a contended vCPU0 BQL.
 * On hosts where a microsecond-scale sleep incurs a millisecond-scale wakeup
 * (Android), most waits end within the budget. No scheduler/affinity change.
 * On by default on Android with the 150us budget: Poco F3, Jetpack title,
 * three interleaved pairs each, off -> 50us 53.42 -> 57.47 fps (hitches
 * 30 -> 8 per 180 s), 50 -> 150us 57.16 -> 58.36 fps (hitches 16 -> 11).
 * LT_BQL_SPIN=off/on and LT_BQL_SPIN_EXTENDED=off/on override the default.
 */
#define BQL_SPIN_NS 50000
#define BQL_SPIN_EXTENDED_NS 150000
static int bql_spin_budget_ns = BQL_SPIN_NS;
static bool bql_spin_on;
static bool bql_spin_fallback_prof;
static uint64_t bql_spin_profiled_fallbacks;
static uint64_t bql_spin_contended, bql_spin_acquired, bql_spin_fallback;

static void bql_spin_lock(QemuMutex *mutex, const char *file, int line)
{
    CPUState *cpu = current_cpu;

    if (!cpu || (cpu->cpu_index != 0 && !cpu->bql_spin)) {
        qemu_mutex_lock_impl(mutex, file, line);
        return;
    }
    if (!qemu_mutex_trylock_impl(mutex, file, line)) {
        return;
    }
    bql_spin_contended++;
    int64_t deadline = get_clock() + bql_spin_budget_ns;
    while (get_clock() < deadline) {
        cpu_relax();
        if (!qemu_mutex_trylock_impl(mutex, file, line)) {
            bql_spin_acquired++;
            return;
        }
    }
    bql_spin_fallback++;
    /* Independent diagnostic, only on the uncommon blocking path. Conditions
     * mode keeps this BQL hook intact; reuse its bounded site/span recorder.
     * The interval excludes the preceding spin. Off adds no clocks. */
    int64_t begin = bql_spin_fallback_prof && native_wait_on ? get_clock() : 0;
    qemu_mutex_lock_impl(mutex, file, line);
    if (begin) {
        bql_spin_profiled_fallbacks++;
        native_wait_end(begin, 1, mutex, mutex, file, line);
    }
}

static void bql_spin_init(void)
{
#ifdef __ANDROID__
    const bool default_on = true;
#else
    const bool default_on = false;
#endif
    const char *mode = getenv("LT_BQL_SPIN");
    const char *extended = getenv("LT_BQL_SPIN_EXTENDED");
    bool requested = mode ? !strcmp(mode, "on") : default_on;
    bool extended_requested = extended ? !strcmp(extended, "on") : default_on;

    /* The extended budget only lengthens the bounded retry; the blocking
     * fallback after it is unchanged. */
    if (requested && extended_requested) {
        bql_spin_budget_ns = BQL_SPIN_EXTENDED_NS;
    }

    if (requested && qatomic_read(&bql_mutex_lock_func) == qemu_mutex_lock_impl) {
        bql_spin_on = true;
        qatomic_set(&bql_mutex_lock_func, bql_spin_lock);
    }
    if (requested) {
        fprintf(stderr, "[bql-spin] active=%s cpu=0 budget-ns=%d\n",
                bql_spin_on ? "on" : "off", bql_spin_budget_ns);
    }
    const char *profile = getenv("LT_BQL_FALLBACK_PROF");
    const char *wait = getenv("LT_NATIVE_WAIT_PROF");
    bool profile_requested = profile && !strcmp(profile, "on");
    bql_spin_fallback_prof = bql_spin_on && profile_requested && wait &&
                             !strcmp(wait, "conditions");
    if (profile_requested) {
        fprintf(stderr, "[bql-fallback] requested=on configured=%s; "
                "requires active conditions trace\n",
                bql_spin_fallback_prof ? "on" : "off");
    }
}

void qemu_ios_bql_spin_finish(void)
{
    if (bql_spin_on) {
        fprintf(stderr, "[bql-spin] finished contended=%" PRIu64
                " acquired=%" PRIu64 " fallback=%" PRIu64 " hook=%s\n",
                bql_spin_contended, bql_spin_acquired, bql_spin_fallback,
                qatomic_read(&bql_mutex_lock_func) == bql_spin_lock ?
                "unchanged" : "replaced");
    }
    if (bql_spin_fallback_prof) {
        fprintf(stderr, "[bql-fallback] finished calls=%" PRIu64 " trace=%s\n",
                bql_spin_profiled_fallbacks, native_wait_on ? "on" : "off");
    }
}
