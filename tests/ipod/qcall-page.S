@ QEMU_CALL request blocks across pages, for tests/ipod/qcall-page-test.sh: a bare-metal program run as the bootrom
@ (at 0, SVC mode, MMU off) on a board whose RAM starts at RAM (-DRAM=). It maps four test pages at 0x70000000 (two
@ whose physical pages are in the opposite order, a hole, one privileged read-only, one for the reference block),
@ issues QEMU_CALL (mcr p15, 3, rX, c15, c15, 0) with a QC_GLES_PING block in each place and compares each reply with
@ the reference's (from User mode, the read-only page gets none). One character per check on the semihosting
@ console: its letter in lowercase if it passed, uppercase if not; then 'E', and it exits.
        .syntax unified
        .arch armv6k
        .arm
        .text
        .equ L1, RAM + 0x20000
        .equ L2, RAM + 0x28000          @ VA 0x70000000-0x700fffff
        .equ VA, 0x70000000
        .equ PG_RW, 0x072               @ small page: AP=11, TEX=001, global
        .equ PG_RO, 0x252               @ APX=1 AP=01: privileged read-only, no User access
        .equ SECT, 0x1c02               @ section: AP=11, TEX=001, domain 0
        .equ PING, 0x141                @ QC_GLES_PING
        .equ SENT, 0x5a5a5a5a
        .equ REF, VA + 0x4100           @ the reference block, in one page
        .equ CROSS, VA + 0xfd8          @ retval straddles 0x70001000: pages at RAM+0x31000, then RAM+0x30000
        .equ HOLE, VA + 0x1fd8          @ its second page (0x70002000) unmapped
        .equ RO, VA + 0x3100            @ in the read-only page

_start:
        b       .Lreset
        b       .
        bx      lr                      @ SVC: stay in SVC

.Lreset:
        cpsid   aif
        ldr     sp, =RAM + 0xf000
        mov     r10, #0
        @ L1: identity sections for the ROM and 64M of RAM, the L2 table at VA
        ldr     r0, =L1
        mov     r1, #0
        mov     r2, #0x4000
1:      str     r1, [r0], #4
        subs    r2, r2, #4
        bne     1b
        ldr     r0, =L1
        ldr     r1, =SECT
        str     r1, [r0]
        ldr     r1, =RAM | SECT
        add     r0, r0, #(RAM >> 20) * 4
        mov     r2, #64
1:      str     r1, [r0], #4
        add     r1, r1, #0x100000
        subs    r2, r2, #1
        bne     1b
        ldr     r0, =L1 + (VA >> 20) * 4
        ldr     r1, =L2 | 1
        str     r1, [r0]
        ldr     r0, =L2
        mov     r1, #0
        mov     r2, #0x400
1:      str     r1, [r0], #4
        subs    r2, r2, #4
        bne     1b
        ldr     r0, =L2
        ldr     r1, =(RAM + 0x31000) | PG_RW
        str     r1, [r0, #0]
        ldr     r1, =(RAM + 0x30000) | PG_RW
        str     r1, [r0, #4]
        ldr     r1, =(RAM + 0x33000) | PG_RO
        str     r1, [r0, #12]
        ldr     r1, =(RAM + 0x34000) | PG_RW
        str     r1, [r0, #16]
        @ MMU on: domain 0 client, TTBCR 0, TTBR0 = L1
        mov     r0, #1
        mcr     p15, 0, r0, c3, c0, 0       @ DACR
        mov     r0, #0
        mcr     p15, 0, r0, c2, c0, 2       @ TTBCR
        ldr     r0, =L1
        mcr     p15, 0, r0, c2, c0, 0       @ TTBR0
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        mrc     p15, 0, r0, c1, c0, 0
        orr     r0, r0, #1                  @ M
        orr     r0, r0, #0x800000           @ XP (v6; RES1 on v7)
        bic     r0, r0, #0x30000000         @ no TRE/AFE
        bic     r0, r0, #0x2000             @ low vectors
        mcr     p15, 0, r0, c1, c0, 0
        mcr     p15, 0, r0, c7, c5, 4       @ ISB

        @ a: the reference call, in one page, gets a reply
        ldr     r0, =REF
        bl      .Lfill
        ldr     r0, =REF
        mcr     p15, 3, r0, c15, c15, 0
        ldr     r1, =REF
        ldr     r0, [r1, #36]
        ldr     r1, =SENT
        cmp     r0, r1
        moveq   r0, #1                      @ still the sentinel: fail
        movne   r0, #0
        mov     r1, #0
        mov     r2, #'a'
        bl      .Lcheck

        @ b: across a page boundary whose physical pages are out of order
        ldr     r0, =CROSS
        bl      .Lfill
        ldr     r0, =CROSS
        mcr     p15, 3, r0, c15, c15, 0
        ldr     r0, =CROSS
        mov     r2, #'b'
        bl      .Lsame

        @ c: the same from User mode
        ldr     r0, =CROSS
        bl      .Lfill
        ldr     r0, =CROSS
        msr     cpsr_c, #0xd0
        mcr     p15, 3, r0, c15, c15, 0
        svc     #0
        ldr     r0, =CROSS
        mov     r2, #'c'
        bl      .Lsame

        @ d: its second page unmapped: no reply, and no fault
        ldr     r0, =HOLE
        mov     r1, #0
        mov     r2, #40                     @ up to the retval's first word, the last in the page
        bl      .Lfillsome
        ldr     r0, =HOLE
        mcr     p15, 3, r0, c15, c15, 0
        ldr     r1, =HOLE
        ldr     r0, [r1, #36]
        ldr     r1, =SENT
        mov     r2, #'d'
        bl      .Lcheck

        @ e: in a read-only page: the reply is written regardless, as a debug write does
        ldr     r0, =RO - VA + RAM + 0x30000 @ through its physical alias
        bl      .Lfill
        ldr     r0, =RO
        mcr     p15, 3, r0, c15, c15, 0
        ldr     r0, =RO
        mov     r2, #'e'
        bl      .Lsame

        @ f: the same block from User mode, which may not touch the page: no reply
        ldr     r0, =RO - VA + RAM + 0x30000
        bl      .Lfill
        ldr     r0, =RO
        msr     cpsr_c, #0xd0
        mcr     p15, 3, r0, c15, c15, 0
        svc     #0
        ldr     r1, =RO - VA + RAM + 0x30000
        ldr     r0, [r1, #36]
        ldr     r1, =SENT
        mov     r2, #'f'
        bl      .Lcheck

        mov     r0, #'E'
        bl      .Lputc
        mov     r0, #'\n'
        bl      .Lputc
        mov     r0, #0x18                   @ SYS_EXIT
        ldr     r1, =0x20026                @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

@ r0 = block: call number PING, args zero, retval and error the sentinel
.Lfill:
        mov     r2, #52
@ r0 = block, r2 = bytes to fill (a multiple of 4)
.Lfillsome:
        push    {r4, lr}
        mov     r4, #0
        ldr     r1, =PING
        str     r1, [r0]
        mov     r1, #0
1:      add     r4, r4, #4
        cmp     r4, r2
        popeq   {r4, pc}
        cmp     r4, #36
        ldrge   r1, =SENT
        str     r1, [r0, r4]
        b       1b

@ r0 = block, r2 = the check's letter: its 52 bytes are the reference's
.Lsame:
        push    {r4, r5, lr}
        ldr     r1, =REF
        mov     r4, #0
        mov     r5, #0
1:      ldr     r3, [r0, r4]
        ldr     r12, [r1, r4]
        cmp     r3, r12
        addne   r5, r5, #1
        add     r4, r4, #4
        cmp     r4, #52
        bne     1b
        mov     r0, r5
        mov     r1, #0
        bl      .Lcheck
        pop     {r4, r5, pc}

@ r0 = got, r1 = expected, r2 = the check's letter
.Lcheck:
        cmp     r0, r1
        subne   r2, r2, #0x20
        addne   r10, r10, #1
        mov     r0, r2
@ r0 = character, to the semihosting console (SYS_WRITEC)
.Lputc:
        push    {r0, r1, r2, r3, r12, lr}
        mov     r1, sp
        mov     r0, #3
        svc     0x123456
        pop     {r0, r1, r2, r3, r12, pc}

        .ltorg
