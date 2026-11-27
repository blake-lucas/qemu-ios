/*
 * sigaltstack coroutine initialization code
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2011  Kevin Wolf <kwolf@redhat.com>
 * Copyright (C) 2012  Alex Barcelo <abarcelo@ac.upc.edu>
** This file is partly based on pth_mctx.c, from the GNU Portable Threads
**  Copyright (c) 1999-2006 Ralf S. Engelschall <rse@engelschall.com>
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

/* XXX Is there a nicer way to disable glibc's stack check for longjmp? */
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0

#include "qemu/osdep.h"
#include <pthread.h>
#include "qemu/coroutine_int.h"

#ifdef CONFIG_SAFESTACK
#error "SafeStack is not compatible with code run in alternate signal stacks"
#endif

#if defined(__ANDROID__) && defined(__aarch64__)
/*
 * Android arm64 switches coroutines with a context switch of its own, not bionic's sigsetjmp/siglongjmp. A
 * coroutine can resume on another thread than the one it last ran on (a thread-pool completion re-enters it in the
 * main loop after a vCPU thread started it), and bionic's jmp_buf carries the thread it was made on: it keeps x18
 * (the shadow call stack) and authenticates the saved return address with pointer authentication, so on Android 17 a
 * Pixel 10's longjmp into a buffer another thread made fails its check (SIGILL, PAC exception in _longjmp). This saves
 * only what the AAPCS64 says a call keeps (x19-x30, sp, d8-d15); x18 stays the running thread's. No signal mask is
 * saved, as with sigsetjmp(env, 0).
 */
typedef uint64_t co_jmp_buf[21];

int lt_co_setjmp(co_jmp_buf env) __attribute__((returns_twice));
void lt_co_longjmp(co_jmp_buf env, int value) __attribute__((noreturn));

__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl lt_co_setjmp\n"
    ".hidden lt_co_setjmp\n"
    ".type lt_co_setjmp, %function\n"
    "lt_co_setjmp:\n"
    "    hint #34\n" /* bti c */
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x9, sp\n"
    "    str x9, [x0, #96]\n"
    "    stp d8, d9, [x0, #104]\n"
    "    stp d10, d11, [x0, #120]\n"
    "    stp d12, d13, [x0, #136]\n"
    "    stp d14, d15, [x0, #152]\n"
    "    mov w0, #0\n"
    "    ret\n"
    ".size lt_co_setjmp, . - lt_co_setjmp\n"
    ".p2align 2\n"
    ".globl lt_co_longjmp\n"
    ".hidden lt_co_longjmp\n"
    ".type lt_co_longjmp, %function\n"
    "lt_co_longjmp:\n"
    "    hint #34\n" /* bti c */
    "    ldp x19, x20, [x0, #0]\n"
    "    ldp x21, x22, [x0, #16]\n"
    "    ldp x23, x24, [x0, #32]\n"
    "    ldp x25, x26, [x0, #48]\n"
    "    ldp x27, x28, [x0, #64]\n"
    "    ldp x29, x30, [x0, #80]\n"
    "    ldr x9, [x0, #96]\n"
    "    mov sp, x9\n"
    "    ldp d8, d9, [x0, #104]\n"
    "    ldp d10, d11, [x0, #120]\n"
    "    ldp d12, d13, [x0, #136]\n"
    "    ldp d14, d15, [x0, #152]\n"
    "    cmp w1, #0\n"
    "    csinc w0, w1, wzr, ne\n" /* longjmp(env, 0) returns 1 */
    "    ret\n"
    ".size lt_co_longjmp, . - lt_co_longjmp\n");

#define co_setjmp(env) lt_co_setjmp(env)
#define co_longjmp(env, value) lt_co_longjmp(env, value)
#else
typedef sigjmp_buf co_jmp_buf;
#define co_setjmp(env) sigsetjmp(env, 0)
#define co_longjmp(env, value) siglongjmp(env, value)
#endif

typedef struct {
    Coroutine base;
    void *stack;
    size_t stack_size;
    co_jmp_buf env;
} CoroutineSigAltStack;

/**
 * Per-thread coroutine bookkeeping
 */
typedef struct {
    /** Currently executing coroutine */
    Coroutine *current;

    /** The default coroutine */
    CoroutineSigAltStack leader;

    /** Information for the signal handler (trampoline) */
    co_jmp_buf tr_reenter;
    volatile sig_atomic_t tr_called;
    void *tr_handler;
} CoroutineThreadState;

static pthread_key_t thread_state_key;

static CoroutineThreadState *coroutine_get_thread_state(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    if (!s) {
        s = g_malloc0(sizeof(*s));
        s->current = &s->leader.base;
        pthread_setspecific(thread_state_key, s);
    }
    return s;
}

static void qemu_coroutine_thread_cleanup(void *opaque)
{
    CoroutineThreadState *s = opaque;

    g_free(s);
}

static void __attribute__((constructor)) coroutine_init(void)
{
    int ret;

    ret = pthread_key_create(&thread_state_key, qemu_coroutine_thread_cleanup);
    if (ret != 0) {
        fprintf(stderr, "unable to create leader key: %s\n", strerror(errno));
        abort();
    }
}

/* "boot" function
 * This is what starts the coroutine, is called from the trampoline
 * (from the signal handler when it is not signal handling, read ahead
 * for more information).
 */
static void coroutine_bootstrap(CoroutineSigAltStack *self, Coroutine *co)
{
    /* Initialize longjmp environment and switch back the caller */
    if (!co_setjmp(self->env)) {
        co_longjmp(*(co_jmp_buf *)co->entry_arg, 1);
    }

    while (true) {
        co->entry(co->entry_arg);
        qemu_coroutine_switch(co, co->caller, COROUTINE_TERMINATE);
    }
}

/*
 * This is used as the signal handler. This is called with the brand new stack
 * (thanks to sigaltstack). We have to return, given that this is a signal
 * handler and the sigmask and some other things are changed.
 */
static void coroutine_trampoline(int signal)
{
    CoroutineSigAltStack *self;
    Coroutine *co;
    CoroutineThreadState *coTS;

    /* Get the thread specific information */
    coTS = coroutine_get_thread_state();
    self = coTS->tr_handler;
    coTS->tr_called = 1;
    co = &self->base;

    /*
     * Here we have to do a bit of a ping pong between the caller, given that
     * this is a signal handler and we have to do a return "soon". Then the
     * caller can reestablish everything and do a siglongjmp here again.
     */
    if (!co_setjmp(coTS->tr_reenter)) {
        return;
    }

    /*
     * Ok, the caller has siglongjmp'ed back to us, so now prepare
     * us for the real machine state switching. We have to jump
     * into another function here to get a new stack context for
     * the auto variables (which have to be auto-variables
     * because the start of the thread happens later). Else with
     * PIC (i.e. Position Independent Code which is used when PTH
     * is built as a shared library) most platforms would
     * horrible core dump as experience showed.
     */
    coroutine_bootstrap(self, co);
}

Coroutine *qemu_coroutine_new(void)
{
    CoroutineSigAltStack *co;
    CoroutineThreadState *coTS;
    struct sigaction sa;
    struct sigaction osa;
    stack_t ss;
    stack_t oss;
    sigset_t sigs;
    sigset_t osigs;
    co_jmp_buf old_env;
    static pthread_mutex_t sigusr2_mutex = PTHREAD_MUTEX_INITIALIZER;

    /* The way to manipulate stack is with the sigaltstack function. We
     * prepare a stack, with it delivering a signal to ourselves and then
     * put sigsetjmp/siglongjmp where needed.
     * This has been done keeping coroutine-ucontext as a model and with the
     * pth ideas (GNU Portable Threads). See coroutine-ucontext for the basics
     * of the coroutines and see pth_mctx.c (from the pth project) for the
     * sigaltstack way of manipulating stacks.
     */

    co = g_malloc0(sizeof(*co));
    co->stack_size = COROUTINE_STACK_SIZE;
    co->stack = qemu_alloc_stack(&co->stack_size);
    co->base.entry_arg = &old_env; /* stash away our jmp_buf */

    coTS = coroutine_get_thread_state();
    coTS->tr_handler = co;

    /*
     * Preserve the SIGUSR2 signal state, block SIGUSR2,
     * and establish our signal handler. The signal will
     * later transfer control onto the signal stack.
     */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &sigs, &osigs);
    sa.sa_handler = coroutine_trampoline;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;

    /*
     * sigaction() is a process-global operation.  We must not run
     * this code in multiple threads at once.
     */
    pthread_mutex_lock(&sigusr2_mutex);
    if (sigaction(SIGUSR2, &sa, &osa) != 0) {
        abort();
    }

    /*
     * Set the new stack.
     */
    ss.ss_sp = co->stack;
    ss.ss_size = co->stack_size;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, &oss) < 0) {
        abort();
    }

    /*
     * Now transfer control onto the signal stack and set it up.
     * It will return immediately via "return" after the sigsetjmp()
     * was performed. Be careful here with race conditions.  The
     * signal can be delivered the first time sigsuspend() is
     * called.
     */
    coTS->tr_called = 0;
    pthread_kill(pthread_self(), SIGUSR2);
    sigfillset(&sigs);
    sigdelset(&sigs, SIGUSR2);
    while (!coTS->tr_called) {
        sigsuspend(&sigs);
    }

    /*
     * Inform the system that we are back off the signal stack by
     * removing the alternative signal stack. Be careful here: It
     * first has to be disabled, before it can be removed.
     */
    sigaltstack(NULL, &ss);
    ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, NULL) < 0) {
        abort();
    }
    sigaltstack(NULL, &ss);
    if (!(oss.ss_flags & SS_DISABLE)) {
        sigaltstack(&oss, NULL);
    }

    /*
     * Restore the old SIGUSR2 signal handler and mask
     */
    sigaction(SIGUSR2, &osa, NULL);
    pthread_mutex_unlock(&sigusr2_mutex);

    pthread_sigmask(SIG_SETMASK, &osigs, NULL);

    /*
     * Now enter the trampoline again, but this time not as a signal
     * handler. Instead we jump into it directly. The functionally
     * redundant ping-pong pointer arithmetic is necessary to avoid
     * type-conversion warnings related to the `volatile' qualifier and
     * the fact that `jmp_buf' usually is an array type.
     */
    if (!co_setjmp(old_env)) {
        co_longjmp(coTS->tr_reenter, 1);
    }

    /*
     * Ok, we returned again, so now we're finished
     */

    return &co->base;
}

void qemu_coroutine_delete(Coroutine *co_)
{
    CoroutineSigAltStack *co = DO_UPCAST(CoroutineSigAltStack, base, co_);

    qemu_free_stack(co->stack, co->stack_size);
    g_free(co);
}

CoroutineAction qemu_coroutine_switch(Coroutine *from_, Coroutine *to_,
                                      CoroutineAction action)
{
    CoroutineSigAltStack *from = DO_UPCAST(CoroutineSigAltStack, base, from_);
    CoroutineSigAltStack *to = DO_UPCAST(CoroutineSigAltStack, base, to_);
    CoroutineThreadState *s = coroutine_get_thread_state();
    int ret;

    s->current = to_;

    ret = co_setjmp(from->env);
    if (ret == 0) {
        co_longjmp(to->env, action);
    }
    return ret;
}

Coroutine *qemu_coroutine_self(void)
{
    CoroutineThreadState *s = coroutine_get_thread_state();

    return s->current;
}

bool qemu_in_coroutine(void)
{
    CoroutineThreadState *s = pthread_getspecific(thread_state_key);

    return s && s->current->caller;
}

