/* Inject BlockBackend/bitmap writeback failures into the actual stop callback.
 *
 * The failed latch must reach the GUI save/resume gate and survive later flushes.
 * This is a host-error unit check; guest behavior belongs in qtest/boot coverage.
 *
 * SLICE:state hw/arm/s5l8930_iop.c range struct S5L8930IOPState { | \n};
 * SLICE hw/arm/s5l8930_iop.c fn s5l8930_iop_io_failed iop_vm_state
 * SLICE contrib/ios-app/qemu-ios-ui.c fn qemu_ios_ui_storage_failed
 * CFLAGS -Wall -Werror
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#define NAND_BUSES 2
#define NAND_CES 8
#define IOP_VIC_COUNT 4
#define IOP_VIC_REGS 0x1000
#define IOP_MAX_ENDPOINTS 8
typedef int SysBusDevice, MemoryRegion, DeviceState, QEMUTimer, RunState, BlockBackend, VMChangeStateEntry;
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
static bool iop_storage_failed;
static unsigned calls, fail_at, errors, successes;
static BlockBackend *page_backend, *bitmap_backend;
static unsigned page_flushes, bitmap_writes;
static int blk_pwrite(BlockBackend *p, int64_t offset, int64_t size, const void *buf, int flags) {
    assert(p == bitmap_backend && offset == 0 && size && buf && flags == 0);
    assert(page_flushes == 1); /* all data durable before ownership */
    ++bitmap_writes;
    if (++calls == fail_at) { return -ENOSPC; }
    return 0;
}
static int blk_flush(BlockBackend *p) {
    assert(p == page_backend || p == bitmap_backend);
    if (++calls == fail_at) { return -ENOSPC; }
    if (p == page_backend) { ++page_flushes; }
    return 0;
}
/* The direct page descriptors (LT_NAND_DIRECT): synced after the BlockBackend flush, before ownership. */
static int sync_fd = -1, sync_fail;
static unsigned fd_syncs;
static int qemu_fdatasync(int fd) {
    assert(fd == sync_fd && page_flushes == 1 && bitmap_writes == 0);
    ++fd_syncs;
    if (sync_fail) { errno = EIO; return -1; }
    return 0;
}
#define MS_SYNC 4
static int msync(void *p, size_t n, int f) { assert(!"dirty[] is not mapped here"); return 0; }
static int64_t g_get_monotonic_time(void) { return 0; }
#define error_report(...) (++errors)
#define info_report(...) ((void)t0, ++successes)
static bool ipod_touch_fmss_io_failed(void) { return false; }
static bool ipod_touch_nor_io_failed(void) { return false; }
static bool nand_image_io_failed(void) { return false; }
#include <stdio.h>
/* The production state is a named struct, used through a typedef in QOM. */
typedef struct S5L8930IOPState S5L8930IOPState;
#include "state.h"
};
#include "slice.h"

int main(void) {
    BlockBackend page, owner; uint8_t bitmap[8];
    page_backend = &page; bitmap_backend = &owner;
    S5L8930IOPState s = {0};
    for (int b = 0; b < NAND_BUSES; b++) for (int c = 0; c < NAND_CES; c++) s.chip_fd[b][c] = s.ovl_fd[b][c] = -1;
    s.pages_per_ce = 64; s.page_stride = 1;
    s.overlay_dir = "overlay";
    s.ovl[0][0] = &page; s.dirty[0][0] = bitmap; s.ownership[0][0] = &owner; s.ownership_pending[0][0] = true;
    iop_vm_state(&s, true, 0);
    assert(calls == 0 && !qemu_ios_ui_storage_failed());
    iop_vm_state(&s, false, 0);
    assert(calls == 3 && successes == 1 && !qemu_ios_ui_storage_failed());
    /* Each failure independently reaches the GUI guard, without success. */
    for (unsigned failure = 1; failure <= 3; ++failure) {
        calls = errors = successes = page_flushes = bitmap_writes = 0; iop_storage_failed = false;
        fail_at = failure; s.ownership_pending[0][0] = true;
        iop_vm_state(&s, false, 0);
        assert(calls == failure && errors == 2 && successes == 0);
        assert(bitmap_writes == (failure == 1 ? 0 : 1));
        assert(qemu_ios_ui_storage_failed());
        fail_at = 0; page_flushes = 0;
        iop_vm_state(&s, false, 0);
        assert(qemu_ios_ui_storage_failed());
    }
    /* A direct descriptor is synced before ownership is published; its failure latches like a flush's. */
    calls = errors = successes = page_flushes = bitmap_writes = 0; iop_storage_failed = false;
    sync_fd = s.ovl_fd[0][0] = 7; s.ownership_pending[0][0] = true;
    iop_vm_state(&s, false, 0);
    assert(fd_syncs == 1 && bitmap_writes == 1 && successes == 1 && !qemu_ios_ui_storage_failed());
    calls = errors = successes = page_flushes = bitmap_writes = 0; sync_fail = 1; s.ownership_pending[0][0] = true;
    iop_vm_state(&s, false, 0);
    assert(fd_syncs == 2 && bitmap_writes == 0 && successes == 0 && errors == 2 && qemu_ios_ui_storage_failed());
    assert(s.ownership_pending[0][0]);
    puts("PASS: IOP page/bitmap flush failures reach GUI; failed session remains failed");
}
