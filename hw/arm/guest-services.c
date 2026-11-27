/*
 * QEMU TCP Tunnelling
 *
 * Copyright (c) 2019 Lev Aronsky <aronsky@gmail.com>
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
#include "qemu/guest-random.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "system/address-spaces.h"
#include "hw/misc/unimp.h"
#include "system/system.h"
#include "qemu/error-report.h"
#include "hw/core/platform-bus.h"
#include "hw/arm/ipod_touch_2g.h"
#include "cpu.h"
#include "qemu/main-loop.h"
#include "accel/tcg/probe.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"

int32_t guest_svcs_errno = 0;

uint64_t qemu_call_status(CPUARMState *env, const struct ARMCPRegInfo *ri)
{
    // NOT USED FOR NOW
    return 0;
}

/* LT_QCALL_FAST=off: no host-pointer path for guest memory, only the debug access (A/B). */
static bool qcall_fast(void)
{
    static int fast = -1;

    if (fast < 0) {
        const char *v = getenv("LT_QCALL_FAST");
        fast = !(v && !strcmp(v, "off"));
    }
    return fast;
}

/*
 * Guest memory for a guest-services caller, at the caller's privilege: the
 * vCPU's TLB as a load or store at its current MMU index would see it. Plain
 * RAM goes through the host pointers (guest_rw_ram); a page the caller may
 * access that isn't plain RAM (MMIO, dirty tracking, a watchpoint) takes a
 * debug access. A page the caller can't access at its level is refused for an
 * unprivileged caller, so User code can't reach kernel or read-only pages
 * through the host; a privileged caller (the kernel, which owns the page
 * tables anyway) gets the debug access, as before. Returns 0 or -1.
 */
int guest_copy(void *opaque, uint32_t address, uint8_t *data, size_t length,
               bool write)
{
    CPUState *cpu = opaque;
    CPUArchState *env = cpu_env(cpu);
    MMUAccessType type = write ? MMU_DATA_STORE : MMU_DATA_LOAD;
    int idx = cpu_mmu_index(cpu, false);
    bool user = (env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_USR;
    vaddr p = address, last = (vaddr)address + length - 1;
    void *host;

    if (!length) {
        return 0;
    }
    if (last > UINT32_MAX) {
        return -1;
    }
    if (qcall_fast() && guest_rw_ram(cpu, address, data, length, write)) {
        return 0;
    }
    for (;;) {
        if (probe_access_flags(env, p, 0, type, idx, true, &host, 0) &
            TLB_INVALID_MASK) {
            if (user) {
                return -1;
            }
            break;
        }
        if ((p & TARGET_PAGE_MASK) == (last & TARGET_PAGE_MASK)) {
            break;
        }
        p = (p & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
    }
    /* bounded: data is the caller's, length bytes long */
    return cpu_memory_rw_debug(cpu, address, data, length, write) ? -1 : 0;
}

/*
 * The request block, 52 bytes on the caller's stack, read and written back once per call: GL calls
 * come thousands of times a second, and a debug access walks the guest page tables and dispatches
 * through the address space each time (~12% of the vCPU in a draw-call benchmark on a Poco F3).
 * Through guest_copy. LT_QCALL_FAST=off skips the host-pointer
 * path and always takes the debug access (A/B).
 */
static int qcall_rw(CPUState *cpu, uint64_t va, void *buf, size_t len, bool write)
{
    if (va > UINT32_MAX) {
        return -1;
    }
    return guest_copy(cpu, va, buf, len, write);
}

/* The other boards' QEMU_CALL handlers read and write the request block through this. */
int qemu_call_rw(CPUState *cpu, uint64_t va, qemu_call_t *q, bool write)
{
    return qcall_rw(cpu, va, q, sizeof(*q), write);
}

/*
 * GL calls arrive thousands of times a second and touch only the GL bridge, which the vCPU that
 * makes them alone drives, so every board's QEMU_CALL runs them without the BQL (a register that is
 * ARM_CP_IO already holds it). Context setup and teardown change the migration blockers and every
 * other call reaches machine state the main loop shares, so those take it.
 */
bool qemu_call_needs_bql(const qemu_call_t *q)
{
    return q->call_number != QC_GLES ||
           (q->args.gles.slot >= GLES_OP_NEW_SHAREGROUP &&
            q->args.gles.slot <= GLES_OP_DELETE_CONTEXT);
}

void qemu_call(CPUARMState *env, const struct ARMCPRegInfo *ri, uint64_t value)
{
    CPUState *cpu = qemu_get_cpu(0);
    qemu_call_t qcall;

/*    static uint8_t hooks_installed = false;

    if (!value) {
        // Special case: not a regular QEMU call. This is used by our
        // kernel task port patch to notify of the readiness for the
        // hook installation.

        IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
        KernelTrHookParams *hook = &nms->hook;

        if (0 != hook->va) {
            //install the hook here because we need the MMU to be already
            //configured and all the memory mapped before installing the hook
            xnu_hook_tr_copy_install(hook->va, hook->pa, hook->buf_va,
                                     hook->buf_pa, hook->code, hook->code_size,
                                     hook->buf_size, hook->scratch_reg);

        }

        if (!hooks_installed) {
            for (i = 0; i < nms->hook_funcs_count; i++) {
                xnu_hook_tr_copy_install(nms->hook_funcs[i].va,
                                         nms->hook_funcs[i].pa,
                                         nms->hook_funcs[i].buf_va,
                                         nms->hook_funcs[i].buf_pa,
                                         nms->hook_funcs[i].code,
                                         nms->hook_funcs[i].code_size,
                                         nms->hook_funcs[i].buf_size,
                                         nms->hook_funcs[i].scratch_reg);
            }
            hooks_installed = true;
        }

        //emulate original opcode: str x20, [x23]
        value = env->xregs[20];
        cpu_memory_rw_debug(cpu, env->xregs[23], (uint8_t*) &value,
                            sizeof(value), 1);

        return;
    }
    */
    // Read the request
    if (qcall_rw(cpu, value, &qcall, sizeof(qcall), 0)) {
        return;
    }

    bool bql = qemu_call_needs_bql(&qcall) && !bql_locked();

    if (bql) {
        bql_lock();
    }

    guest_svcs_errno = 0;
    switch (qcall.call_number) {
        case QC_GLES:
            qcall.retval = qc_handle_gles(cpu, &qcall.args.gles);
            break;
        case QC_GLES_PING:
            // Proves the guest reached the host from whatever privilege level
            // it issued the mcr at. Nothing else here can answer that question:
            // an unhandled cp15 write on this machine is silently discarded, so
            // "no effect" and "no trap" look identical from the guest.
            qcall.retval = QC_GLES_PING_MAGIC;
            break;
        case QC_PEEK_INPUT: {
            IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
            qcall.retval = nms->kbd_head != nms->kbd_tail;
            break;
        }
        case QC_POLL_INPUT: {
            // Dequeue one host-keyboard unichar for the guest text-input agent.
            IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
            if (nms->kbd_head != nms->kbd_tail) {
                qcall.retval = nms->kbd_ring[nms->kbd_head];
                nms->kbd_head = (nms->kbd_head + 1) % ARRAY_SIZE(nms->kbd_ring);
            } else {
                qcall.retval = 0;
            }
            break;
        }
        /*
         * Pasteboard. The text never travels in the args union -- that is
         * frozen at 32 bytes and widening it would silently move retval out
         * from under the already-deployed keyboard agent -- so it is windowed
         * through a guest buffer instead.
         */
        case QC_AG_HELLO:
        case QC_AG_POLL:
        case QC_AG_READ:
        case QC_AG_WRITE:
        case QC_AG_DONE:
        case QC_AG_HOSTTIME:
        case QC_UI_POLL:
        case QC_UI_READ:
        case QC_UI_WRITE:
        case QC_UI_DONE:
        case QC_AG_UI_ROUTE: {
            IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
            uint64_t candidate = 0;
            if (qcall.call_number == QC_AG_HELLO || qcall.call_number == QC_AG_UI_ROUTE) {
                qemu_guest_getrandom_nofail(&candidate, sizeof(candidate));
            }
            qcall.retval = ipod_agent_call(nms->agent, qcall.call_number,
                qcall.args.ag.token, qcall.args.ag.buffer_guest_ptr,
                qcall.args.ag.offset, qcall.args.ag.length,
                qemu_clock_get_ms(QEMU_CLOCK_REALTIME), candidate,
                guest_copy, cpu);
            guest_svcs_errno = qcall.retval < 0 ? EINVAL : 0;
            break;
        }
        case QC_GLES_HELLO:
        case QC_PKG_OFFER:
        case QC_PKG_READ:
        case QC_PKG_REPORT: {
            IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
            guest_pkg_call(&nms->pkg, cpu, &qcall, &guest_svcs_errno);
            break;
        }
        case QC_PB_POLL:
        case QC_PB_READ:
        case QC_PB_ACK:
        case QC_PB_WRITE:
        case QC_PB_COMMIT: {
            IPodTouchMachineState *nms = IPOD_TOUCH_MACHINE(qdev_get_machine());
            guest_pb_call(&nms->pb, cpu, &qcall, &guest_svcs_errno);
            break;
        }
        default:
            /* Retired host-file opcodes also arrive here. No host files were
             * registered; their old handlers aborted on every guest request. */
            qcall.retval = -1;
            guest_svcs_errno = QC_ERR_ENOSYS;
            break;
    }

    qcall.error = guest_svcs_errno;

    // Write the response
    qcall_rw(cpu, value, &qcall, sizeof(qcall), 1);
    if (bql) {
        bql_unlock();
    }
}
