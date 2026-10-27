/*
 * A NAND image store: one raw file per chip select in a directory, written in
 * place. geometry.json names the layout; record `block * pages_per_block +
 * page` sits at `record * (page_bytes + spare_bytes)`, data then spare, and is
 * stored XOR the model's blank record, so a hole (a sparse range, or an erase)
 * reads as the blank page. Host independent: the files go through QEMU's block
 * layer (an erase unmaps where the host can: F_PUNCHHOLE on macOS, fallocate
 * on Linux). A NAND container (below) holds every chip in one qcow2 disk.
 *
 *   geometry.json  {"storage_format": "nand-xor-blank-v2", "blank": "ff" | "n72-generated",
 *                   "chip_selects", "page_bytes", "spare_bytes", "pages_per_block", "blocks_per_ce"}
 *   cs<N>.pages    one per chip select, pages_per_ce * stride bytes
 */
#ifndef HW_ARM_NAND_IMAGE_H
#define HW_ARM_NAND_IMAGE_H

#include "system/block-backend.h"

#define NAND_IMAGE_FORMAT "nand-xor-blank-v2"
#define NAND_IMAGE_MAX_CHIPS 8

typedef struct NANDImage NANDImage;

/* The geometry a model expects; nand_image_open refuses a store that differs. */
typedef struct NANDImageGeometry {
    unsigned chips;
    uint32_t page_bytes, spare_bytes, pages_per_block, blocks_per_chip;
} NANDImageGeometry;

/* One raw file as a BlockBackend owned by `owner`, `size` bytes (a writable
 * empty file is sized). Shared with the A4 IOP's stores. */
BlockBackend *nand_image_open_file(DeviceState *owner, const char *path,
                                   size_t size, bool writable, Error **errp);

/* Whether `dir` holds a NAND image store (geometry.json names this format). */
bool nand_image_is_store(const char *dir);

/* The store in `dir`, opened for writing; NULL (errp set) when it is not one
 * of `geo`. Flushed on every VM stop. A container (below) needs `overlay`, the
 * device's writable directory; a store of per-chip files is written in place
 * and takes none. */
NANDImage *nand_image_open(DeviceState *owner, const char *dir, const char *overlay,
                           const NANDImageGeometry *geo, Error **errp);

/* One record (page_bytes + spare_bytes) as the chip holds it. 0 or -errno. */
int nand_image_read(NANDImage *n, unsigned chip, uint32_t page, uint8_t *record);
/* Replaces one record. 0 or -errno. */
int nand_image_write(NANDImage *n, unsigned chip, uint32_t page, const uint8_t *record);
/* Every record of one erase block reads blank again. 0 or -errno. */
int nand_image_erase(NANDImage *n, unsigned chip, uint32_t block);

/*
 * A NAND container: geometry.json also names
 *
 *   "container": "qcow2", "block_stride": B, "chip_stride": C
 *
 * and every chip lives in one virtual disk, nand.qcow2: chip c's record p at
 * c * C + (p / pages_per_block) * B + (p % pages_per_block) * stride, so an
 * erase block starts on a 64 KiB cluster and its erase frees whole clusters.
 * The directory's nand.qcow2 is the system image the preparer wrote; it is
 * only read. The guest writes into the device's own overlay, <overlay>/nand.qcow2
 * (qcow2, 64 KiB clusters), made on first use with the
 * system image as its backing file. The backing is given at open, so a device
 * folder can move; the header's relative name is for qemu-img. An erase unmaps
 * the overlay's clusters and punches them out of its file (discard=unmap).
 *
 * A program's data reaches the host's page cache before it returns; the qcow2
 * metadata of a burst of programs that allocated clusters follows when the burst
 * ends (a main-loop bottom half), and an erase's before it returns.
 * nand_disk_sync makes it all durable.
 */
typedef struct NANDDisk NANDDisk;

/* Whether `dir`'s geometry.json names a container. */
bool nand_disk_is_container(const char *dir);

/* The container in `dir` under the overlay in `overlay_dir` (made if missing);
 * NULL (errp set) when its layout cannot hold `chips` x `blocks_per_chip`
 * blocks of `pages_per_block` records of `stride` bytes. */
NANDDisk *nand_disk_open(DeviceState *owner, const char *dir, const char *overlay_dir, unsigned chips,
                         uint32_t stride, uint32_t pages_per_block, uint32_t blocks_per_chip, Error **errp);
/* Where chip `chip`'s record `page` starts in the disk. */
uint64_t nand_disk_record(NANDDisk *d, unsigned chip, uint32_t page);
/* `len` bytes at `offset`: 0 or -errno. */
int nand_disk_read(NANDDisk *d, uint64_t offset, uint32_t len, void *buf);
int nand_disk_write(NANDDisk *d, uint64_t offset, uint32_t len, const void *buf);
/* The whole erase block reads as zeros; its clusters are freed. 0 or -errno. */
int nand_disk_erase(NANDDisk *d, unsigned chip, uint32_t block);
/* Metadata and data on stable storage. 0 or -errno. */
int nand_disk_sync(NANDDisk *d);
void nand_disk_close(NANDDisk *d, DeviceState *owner);

/* NAND page I/O straight on the host files (POSIX; LT_NAND_DIRECT=off: through the block layer). */
bool nand_direct_enabled(void);

/*
 * Guest storage traffic as the boards' NAND controllers perform it (every board, both NAND formats), for the apps'
 * performance overlay (qemu_ios_storage_stats): operations, the bytes they moved and the host time they took. A
 * controller passes nand_stat_begin()'s value to nand_stat() once the operation is done. Counters only grow.
 */
typedef enum { NAND_STAT_READ, NAND_STAT_WRITE, NAND_STAT_ERASE, NAND_STAT_KINDS } NANDStat;
typedef struct NANDStorageStats {
    uint64_t ops[NAND_STAT_KINDS], bytes[NAND_STAT_KINDS], ns[NAND_STAT_KINDS];
} NANDStorageStats;
int64_t nand_stat_begin(void);
void nand_stat(NANDStat kind, uint64_t bytes, int64_t begin);
void nand_storage_stats(NANDStorageStats *out);

#endif
