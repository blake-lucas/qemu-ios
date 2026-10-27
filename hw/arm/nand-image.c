/*
 * NAND image store: see include/hw/arm/nand-image.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qnum.h"
#include "block/block.h"
#include "block/block_int-common.h"
#include "block/block-io.h"
#include "block/coroutines.h"
#include "system/runstate.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "hw/arm/nand-image.h"
#ifdef _WIN32
#include <winioctl.h>
#endif

struct NANDImage {
    BlockBackend *chip[NAND_IMAGE_MAX_CHIPS];
    NANDDisk *disk;    /* a container: every chip in it, chip[] unused */
    DeviceState *owner;
    NANDImageGeometry geo;
    uint32_t stride;
    uint8_t *blank;    /* what a hole reads as: the stored XOR key */
    uint8_t *buffer;
    VMChangeStateEntry *vm_state;
};

/*
 * Size a new, empty file. On Windows it is made sparse first (an overlay
 * chip is gigabytes the guest mostly never writes), and sized through the
 * handle: MinGW's ftruncate refuses to grow a file past the disk's free
 * space, sparse or not, and crashes where FindFirstVolumeW fails (Wine).
 */
static int nand_image_size_new(int fd, size_t size)
{
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    FILE_END_OF_FILE_INFO end = { .EndOfFile.QuadPart = size };
    DWORD returned;

    DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
    if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof(end))) {
        errno = GetLastError() == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    return 0;
#else
    return ftruncate(fd, size);
#endif
}

BlockBackend *nand_image_open_file(DeviceState *owner, const char *path,
                                   size_t size, bool writable, Error **errp)
{
    struct stat st;
    QDict *options;
    BlockBackend *blk;
    BlockDriverState *bs;
    uint64_t perm = BLK_PERM_CONSISTENT_READ | (writable ? BLK_PERM_WRITE : 0);
    uint64_t shared = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE_UNCHANGED;
    int fd = open(path, (writable ? O_RDWR | O_CREAT : O_RDONLY) | O_BINARY, 0644);

    if (fd < 0 || fstat(fd, &st) < 0 ||
        (writable && st.st_size == 0 && nand_image_size_new(fd, size) < 0)) {
        error_setg_errno(errp, errno, "cannot open NAND file %s", path);
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }
    if (st.st_size != size && !(writable && st.st_size == 0)) {
        error_setg(errp, "%s: NAND file size differs from geometry", path);
        close(fd);
        return NULL;
    }
#ifdef _WIN32
    /* Files an older engine left without the attribute: from here on an
     * erase frees its block's clusters instead of writing zeros into them. */
    if (writable && st.st_size != 0) {
        DWORD returned;

        DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE, NULL, 0,
                        NULL, 0, &returned, NULL);
    }
    /* Windows lacks directory fsync. Flush newly sized data/ownership files
     * before any ownership bit can be published. */
    if (writable && fsync(fd) < 0) {
        error_setg_errno(errp, errno, "cannot sync NAND file %s", path);
        close(fd);
        return NULL;
    }
#endif
    close(fd);
    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    bs = bdrv_open(path, NULL, options, BDRV_O_NO_SHARE |
                   (runstate_check(RUN_STATE_INMIGRATE) ? BDRV_O_INACTIVE : 0) |
                   (writable ? BDRV_O_RDWR | BDRV_O_UNMAP : 0), errp);
    if (!bs) {
        return NULL;
    }
    /* Attach the guest owner before requesting permissions: incoming
     * migration keeps images inactive until normal block-layer handoff. */
    blk = blk_new(bdrv_get_aio_context(bs), 0, shared);
    if (blk_attach_dev(blk, owner) < 0) {
        error_setg(errp, "%s: NAND backend already has a device owner", path);
        blk_unref(blk);
        bdrv_unref(bs);
        return NULL;
    }
    if (blk_set_perm(blk, perm, shared, errp) < 0 ||
        blk_insert_bs(blk, bs, errp) < 0) {
        blk_detach_dev(blk, owner);
        blk_unref(blk);
        bdrv_unref(bs);
        return NULL;
    }
    bdrv_unref(bs);
    return blk;
}

static QDict *nand_image_geometry(const char *dir, Error **errp)
{
    g_autofree char *path = g_build_filename(dir, "geometry.json", NULL);
    g_autofree char *json = NULL;
    QObject *obj;
    QDict *g;

    if (!g_file_get_contents(path, &json, NULL, NULL)) {
        error_setg(errp, "cannot read %s", path);
        return NULL;
    }
    obj = qobject_from_json(json, NULL);
    g = qobject_to(QDict, obj);
    if (!g) {
        error_setg(errp, "%s: not a JSON object", path);
        qobject_unref(obj);
    }
    return g;
}

bool nand_image_is_store(const char *dir)
{
    QDict *g = nand_image_geometry(dir, NULL);
    const char *format = g ? qdict_get_try_str(g, "storage_format") : NULL;
    bool is = format && !strcmp(format, NAND_IMAGE_FORMAT);

    qobject_unref(g);
    return is;
}

/* The flash's erased record, as the store's "blank" names it. */
static bool nand_image_blank(const char *name, uint8_t *record, uint32_t page_bytes, uint32_t spare_bytes)
{
    if (!g_strcmp0(name, "ff")) {
        memset(record, 0xff, page_bytes + spare_bytes);
        return true;
    }
    /* The iPod touch 2G's generated layout (ipod_touch_fmss.c fmss_blank_page): zero data, spare word 2 0x00FF00FF. */
    if (!g_strcmp0(name, "n72-generated") && spare_bytes >= 12) {
        memset(record, 0, page_bytes + spare_bytes);
        stl_le_p(record + page_bytes + 8, 0x00ff00ff);
        return true;
    }
    return false;
}

static void nand_image_vm_state(void *opaque, bool running, RunState state)
{
    NANDImage *n = opaque;

    if (running) {
        return;
    }
    if (n->disk) {
        int result = nand_disk_sync(n->disk);
        if (result < 0) {
            error_report("NAND image: sync: %s", strerror(-result));
        }
        return;
    }
    for (unsigned i = 0; i < n->geo.chips; i++) {
        int result = blk_flush(n->chip[i]);
        if (result < 0) {
            error_report("NAND image: flush of chip select %u: %s", i, strerror(-result));
        }
    }
}

static void nand_image_free(NANDImage *n)
{
    nand_disk_close(n->disk, n->owner);
    for (unsigned i = 0; i < NAND_IMAGE_MAX_CHIPS; i++) {
        if (n->chip[i]) {
            blk_unref(n->chip[i]);
        }
    }
    g_free(n->blank);
    g_free(n->buffer);
    g_free(n);
}

NANDImage *nand_image_open(DeviceState *owner, const char *dir, const char *overlay,
                           const NANDImageGeometry *geo, Error **errp)
{
    g_autoptr(QDict) g = nand_image_geometry(dir, errp);
    const char *keys[] = { "chip_selects", "page_bytes", "spare_bytes", "pages_per_block", "blocks_per_ce" };
    const uint32_t want[] = { geo->chips, geo->page_bytes, geo->spare_bytes, geo->pages_per_block,
                              geo->blocks_per_chip };
    NANDImage *n;

    if (!g) {
        return NULL;
    }
    if (g_strcmp0(qdict_get_try_str(g, "storage_format"), NAND_IMAGE_FORMAT)) {
        error_setg(errp, "%s: not a " NAND_IMAGE_FORMAT " store", dir);
        return NULL;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(keys); i++) {
        QNum *v = qobject_to(QNum, qdict_get(g, keys[i]));
        uint64_t got;
        if (!v || !qnum_get_try_uint(v, &got) || got != want[i]) {
            error_setg(errp, "%s: geometry.json \"%s\" is not the model's %u", dir, keys[i], want[i]);
            return NULL;
        }
    }
    if (geo->chips > NAND_IMAGE_MAX_CHIPS) {
        error_setg(errp, "%s: %u chip selects is more than %d", dir, geo->chips, NAND_IMAGE_MAX_CHIPS);
        return NULL;
    }
    n = g_new0(NANDImage, 1);
    n->geo = *geo;
    n->stride = geo->page_bytes + geo->spare_bytes;
    n->blank = g_malloc(n->stride);
    n->buffer = g_malloc(n->stride);
    if (!nand_image_blank(qdict_get_try_str(g, "blank"), n->blank, geo->page_bytes, geo->spare_bytes)) {
        error_setg(errp, "%s: geometry.json names no blank record this model knows", dir);
        nand_image_free(n);
        return NULL;
    }
    n->owner = owner;
    if (!g_strcmp0(qdict_get_try_str(g, "container"), "qcow2")) {
        n->disk = nand_disk_open(owner, dir, overlay, geo->chips, n->stride, geo->pages_per_block,
                                 geo->blocks_per_chip, errp);
        if (!n->disk) {
            nand_image_free(n);
            return NULL;
        }
    } else if (overlay) {
        error_setg(errp, "%s is a NAND image store, written in place: it takes no overlay", dir);
        nand_image_free(n);
        return NULL;
    }
    for (unsigned i = 0; i < geo->chips && !n->disk; i++) {
        g_autofree char *path = g_strdup_printf("%s/cs%u.pages", dir, i);
        size_t size = (size_t)geo->blocks_per_chip * geo->pages_per_block * n->stride;
        n->chip[i] = nand_image_open_file(owner, path, size, true, errp);
        if (!n->chip[i]) {
            nand_image_free(n);
            return NULL;
        }
    }
    n->vm_state = qemu_add_vm_change_state_handler(nand_image_vm_state, n);
    return n;
}

static bool nand_image_bad(NANDImage *n, unsigned chip, uint32_t page)
{
    return chip >= n->geo.chips || page / n->geo.pages_per_block >= n->geo.blocks_per_chip;
}

static void nand_image_xor(NANDImage *n, uint8_t *out, const uint8_t *in)
{
    for (uint32_t i = 0; i < n->stride; i++) {
        out[i] = in[i] ^ n->blank[i];
    }
}

static NANDStorageStats nand_stats;

int64_t nand_stat_begin(void)
{
    return get_clock();
}

void nand_stat(NANDStat kind, uint64_t bytes, int64_t begin)
{
    qatomic_add(&nand_stats.ops[kind], 1);
    qatomic_add(&nand_stats.bytes[kind], bytes);
    qatomic_add(&nand_stats.ns[kind], get_clock() - begin);
}

void nand_storage_stats(NANDStorageStats *out)
{
    for (int i = 0; i < NAND_STAT_KINDS; i++) {
        out->ops[i] = qatomic_read(&nand_stats.ops[i]);
        out->bytes[i] = qatomic_read(&nand_stats.bytes[i]);
        out->ns[i] = qatomic_read(&nand_stats.ns[i]);
    }
}

int nand_image_read(NANDImage *n, unsigned chip, uint32_t page, uint8_t *record)
{
    int result;

    if (nand_image_bad(n, chip, page)) {
        return -EINVAL;
    }
    result = n->disk ? nand_disk_read(n->disk, nand_disk_record(n->disk, chip, page), n->stride, record)
                     : blk_pread(n->chip[chip], (int64_t)page * n->stride, n->stride, record, 0);
    if (result < 0) {
        return result;
    }
    nand_image_xor(n, record, record);
    return 0;
}

int nand_image_write(NANDImage *n, unsigned chip, uint32_t page, const uint8_t *record)
{
    if (nand_image_bad(n, chip, page)) {
        return -EINVAL;
    }
    nand_image_xor(n, n->buffer, record);
    if (n->disk) {
        return nand_disk_write(n->disk, nand_disk_record(n->disk, chip, page), n->stride, n->buffer);
    }
    return blk_pwrite(n->chip[chip], (int64_t)page * n->stride, n->stride, n->buffer, 0);
}

int nand_image_erase(NANDImage *n, unsigned chip, uint32_t block)
{
    uint32_t first = block * n->geo.pages_per_block;

    if (nand_image_bad(n, chip, first)) {
        return -EINVAL;
    }
    if (n->disk) {
        return nand_disk_erase(n->disk, chip, block);
    }
    /* Zeros read as blank whether the host unmaps the range or writes them. */
    return blk_pwrite_zeroes(n->chip[chip], (int64_t)first * n->stride,
                             (int64_t)n->geo.pages_per_block * n->stride, BDRV_REQ_MAY_UNMAP);
}

/* ---- containers: a qcow2 system image under the device's qcow2 overlay ---- */

#define NAND_DISK_FILE      "nand.qcow2"
#define NAND_DISK_CLUSTER   65536

struct NANDDisk {
    BlockBackend *blk;                  /* the overlay; its backing is the system image */
    BlockDriverState *top_file;         /* the overlay's host file, as block status names it */
    BlockDriverState *base_file;        /* the system image's */
    int top_fd, base_fd;                /* the same files for direct page I/O; -1 = none */
    int sync_fd;                        /* the overlay's file, for the stop's fdatasync */
    QEMUBH *flush;                      /* writes the metadata of the programs since the last one */
    int flush_error;                    /* its failure, for the stop to report */
    uint64_t chip_stride, block_stride;
    uint32_t stride, pages_per_block, blocks_per_chip;
    unsigned chips;
    struct NANDDiskMapping *map;        /* recent clusters' block status, for direct page I/O */
    uint32_t map_generation;            /* bumped whenever a cluster's mapping may change */
};

/*
 * Both images map whole 64 KiB clusters, so one block status holds for its cluster until the overlay allocates
 * or frees one: a program through qcow2 or an erase. Those bump map_generation, dropping every entry.
 */
#define NAND_DISK_MAP_SLOTS 4096
enum { NAND_DISK_ZERO = 1, NAND_DISK_TOP, NAND_DISK_BASE };
typedef struct NANDDiskMapping {
    uint64_t cluster;
    uint64_t host;                      /* where the cluster starts in its file */
    uint32_t generation;
    uint32_t kind;                      /* NAND_DISK_*; 0: no entry */
} NANDDiskMapping;

bool nand_direct_enabled(void)
{
#ifdef _WIN32
    return false;
#else
    const char *v = getenv("LT_NAND_DIRECT");

    return !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
#endif
}

bool nand_disk_is_container(const char *dir)
{
    QDict *g = nand_image_geometry(dir, NULL);
    bool is = g && !g_strcmp0(qdict_get_try_str(g, "container"), "qcow2");

    qobject_unref(g);
    return is;
}

/* `to` (absolute) as a path relative to the directory `from`, with '/' separators; NULL when they share no root
 * (two Windows drives). */
static char *nand_relative(const char *from, const char *to)
{
    g_autofree char *a = g_canonicalize_filename(from, NULL);
    g_autofree char *b = g_canonicalize_filename(to, NULL);
    g_auto(GStrv) fa = NULL, fb = NULL;
    GString *out = g_string_new(NULL);
    unsigned i = 0;

#ifdef _WIN32
    g_strdelimit(a, "\\", '/');
    g_strdelimit(b, "\\", '/');
#endif
    fa = g_strsplit(a, "/", -1);
    fb = g_strsplit(b, "/", -1);
    while (fa[i] && fb[i] && !strcmp(fa[i], fb[i])) {
        i++;
    }
    if (i == 0) {
        g_string_free(out, true);
        return NULL;
    }
    for (unsigned j = i; fa[j]; j++) {
        if (*fa[j]) {
            g_string_append(out, "../");
        }
    }
    for (unsigned j = i; fb[j]; j++) {
        g_string_append(out, fb[j]);
        if (fb[j + 1]) {
            g_string_append_c(out, '/');
        }
    }
    return g_string_free(out, false);
}

/*
 * A new overlay over `system`, `size` bytes, published under `path` only once complete. 64 KiB clusters, no
 * subclusters: the first program into a cluster allocates all of it (copying the rest from the system image, or
 * zeroing it), and the cluster's later programs are plain writes into the file (nand_disk_write's fast path). With
 * extended L2 entries (2 KiB subclusters) every page program allocated subclusters through qcow2: ~120 us a program
 * against ~12 us, on an iPhone 3GS app install.
 */
static bool nand_disk_create(const char *path, const char *system, uint64_t size, Error **errp)
{
    g_autofree char *dir = g_path_get_dirname(path);
    g_autofree char *tmp = g_strdup_printf("%s.new", path);
    g_autofree char *backing = nand_relative(dir, system);
    g_autofree char *options = g_strdup_printf("cluster_size=%d", NAND_DISK_CLUSTER);
    Error *err = NULL;

    if (g_mkdir_with_parents(dir, 0755) < 0) {
        error_setg_errno(errp, errno, "cannot create %s", dir);
        return false;
    }
    unlink(tmp);
    /* NO_BACKING: the name is recorded, not opened (it is relative to the overlay, not to this process). */
    bdrv_img_create(tmp, "qcow2", backing ? backing : system, "qcow2", options, size, BDRV_O_NO_BACKING, true,
                    &err);
    if (err) {
        error_propagate_prepend(errp, err, "cannot create NAND overlay %s: ", path);
        unlink(tmp);
        return false;
    }
#ifdef _WIN32
    {
        /* Sparse before any cluster lands, so an erase's discard frees the cluster (FSCTL_SET_ZERO_DATA). */
        int fd = open(tmp, O_RDWR | O_BINARY);
        DWORD returned;

        if (fd >= 0) {
            DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
            close(fd);
        }
    }
#endif
    if (rename(tmp, path) < 0) {
        error_setg_errno(errp, errno, "cannot publish NAND overlay %s", path);
        unlink(tmp);
        return false;
    }
    return true;
}

static int nand_disk_fd(const char *path, bool writable)
{
#ifdef O_CLOEXEC
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_BINARY | O_CLOEXEC);
#else
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_BINARY);
#endif

    if (fd < 0) {
        warn_report("NAND disk: %s: %s; its pages go through the block layer", path, strerror(errno));
    }
    return fd;
}

void nand_disk_close(NANDDisk *d, DeviceState *owner)
{
    if (!d) {
        return;
    }
    if (d->flush) {
        qemu_bh_delete(d->flush);
    }
    if (d->blk) {
        if (owner) {
            blk_detach_dev(d->blk, owner);
        }
        blk_unref(d->blk);
    }
    if (d->top_fd >= 0) {
        close(d->top_fd);
    }
    if (d->base_fd >= 0) {
        close(d->base_fd);
    }
    if (d->sync_fd >= 0) {
        close(d->sync_fd);
    }
    g_free(d->map);
    g_free(d);
}

/*
 * After a burst of programs (a guest NAND command's pages), one write of qcow2's dirty metadata to the OS: a
 * killed engine then loses at most the programs of the burst it died in, as power lost mid-command would.
 */
static void nand_disk_flush(void *opaque)
{
    NANDDisk *d = opaque;
    int result = blk_flush(d->blk);

    if (result < 0 && !d->flush_error) {
        d->flush_error = result;
        error_report("NAND disk: metadata write: %s", strerror(-result));
    }
}

NANDDisk *nand_disk_open(DeviceState *owner, const char *dir, const char *overlay_dir, unsigned chips,
                         uint32_t stride, uint32_t pages_per_block, uint32_t blocks_per_chip, Error **errp)
{
    g_autoptr(QDict) g = nand_image_geometry(dir, errp);
    g_autofree char *system = g_build_filename(dir, NAND_DISK_FILE, NULL);
    g_autofree char *top = NULL;
    uint64_t block_stride, chip_stride, size;
    int64_t length;
    QNum *b, *c;
    QDict *o;
    BlockDriverState *bs;
    NANDDisk *d;
    uint64_t shared = BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE_UNCHANGED;

    if (!g) {
        return NULL;
    }
    if (!overlay_dir || !*overlay_dir) {
        error_setg(errp, "%s: a NAND container needs the device's overlay directory", dir);
        return NULL;
    }
    b = qobject_to(QNum, qdict_get(g, "block_stride"));
    c = qobject_to(QNum, qdict_get(g, "chip_stride"));
    if (!b || !c || !qnum_get_try_uint(b, &block_stride) || !qnum_get_try_uint(c, &chip_stride) ||
        block_stride < (uint64_t)pages_per_block * stride || block_stride % NAND_DISK_CLUSTER ||
        chip_stride / block_stride < blocks_per_chip || chip_stride % NAND_DISK_CLUSTER ||
        chip_stride > (UINT64_MAX >> 2) / chips) {
        error_setg(errp, "%s: geometry.json's container layout does not hold this NAND", dir);
        return NULL;
    }
    size = chip_stride * chips;
    top = g_build_filename(overlay_dir, NAND_DISK_FILE, NULL);
    if (!g_file_test(top, G_FILE_TEST_EXISTS) && !nand_disk_create(top, system, size, errp)) {
        return NULL;
    }
    o = qdict_new();
    qdict_put_str(o, "driver", "qcow2");
    qdict_put_str(o, "file.driver", "file");
    qdict_put_str(o, "file.filename", top);
    qdict_put_str(o, "backing.driver", "qcow2");
    qdict_put_str(o, "backing.file.driver", "file");
    qdict_put_str(o, "backing.file.filename", system);
    /* Every L2 table cached (QEMU caps this at what the disk needs: ~4.5 MiB for an iPod touch 4G's overlay),
     * in 4 KiB slices so a page program that allocates writes 4 KiB of L2, not a cluster's worth. */
    qdict_put_str(o, "l2-cache-size", "32M");
    qdict_put_str(o, "l2-cache-entry-size", "4096");
    qdict_put_str(o, "backing.l2-cache-size", "32M");
    qdict_put_str(o, "backing.l2-cache-entry-size", "4096");
    /*
     * NO_FLUSH: a flush writes qcow2's metadata to the OS without an fdatasync, so every program can be
     * followed by one (a killed engine then leaves page data and its mapping together); the stop syncs the file.
     * UNMAP: an erase's zeroes free the clusters and pass the discard down as a hole punch.
     */
    bs = bdrv_open(NULL, NULL, o, BDRV_O_RDWR | BDRV_O_UNMAP | BDRV_O_NO_FLUSH |
                   (runstate_check(RUN_STATE_INMIGRATE) ? BDRV_O_INACTIVE : 0), errp);
    if (!bs) {
        return NULL;
    }
    length = bdrv_getlength(bs);
    if (length < 0 || (uint64_t)length != size || !bs->backing || !bs->backing->bs->file || !bs->file ||
        bdrv_getlength(bs->backing->bs) != length) {
        error_setg(errp, "%s: NAND disk size differs from geometry", top);
        bdrv_unref(bs);
        return NULL;
    }
    d = g_new0(NANDDisk, 1);
    d->top_fd = d->base_fd = d->sync_fd = -1;
    d->blk = blk_new(bdrv_get_aio_context(bs), 0, shared);
    if (blk_attach_dev(d->blk, owner) < 0) {
        error_setg(errp, "%s: NAND backend already has a device owner", top);
        blk_unref(d->blk);
        d->blk = NULL;
        bdrv_unref(bs);
        nand_disk_close(d, NULL);
        return NULL;
    }
    if (blk_set_perm(d->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE, shared, errp) < 0 ||
        blk_insert_bs(d->blk, bs, errp) < 0) {
        bdrv_unref(bs);
        nand_disk_close(d, owner);
        return NULL;
    }
    bdrv_unref(bs);
    d->top_file = bs->file->bs;
    d->base_file = bs->backing->bs->file->bs;
    d->chips = chips;
    d->stride = stride;
    d->pages_per_block = pages_per_block;
    d->blocks_per_chip = blocks_per_chip;
    d->block_stride = block_stride;
    d->chip_stride = chip_stride;
    d->sync_fd = nand_disk_fd(top, true);
#ifdef _WIN32
    if (d->sync_fd >= 0) {
        DWORD returned;

        DeviceIoControl((HANDLE)_get_osfhandle(d->sync_fd), FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);
    }
#endif
    if (nand_direct_enabled()) {
        d->top_fd = nand_disk_fd(top, true);
        d->base_fd = nand_disk_fd(system, false);
        if (d->top_fd >= 0 && d->base_fd >= 0) {
            d->map = g_new0(NANDDiskMapping, NAND_DISK_MAP_SLOTS);
            d->map_generation = 1;
        }
    }
    d->flush = qemu_bh_new(nand_disk_flush, d);
    return d;
}

uint64_t nand_disk_record(NANDDisk *d, unsigned chip, uint32_t page)
{
    return chip * d->chip_stride + (uint64_t)(page / d->pages_per_block) * d->block_stride +
           (uint64_t)(page % d->pages_per_block) * d->stride;
}

#ifndef _WIN32
static int nand_disk_pio(int fd, void *buf, uint32_t len, uint64_t offset, bool write)
{
    uint8_t *p = buf;

    while (len) {
        ssize_t n = write ? pwrite(fd, p, len, offset) : pread(fd, p, len, offset);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return n < 0 ? -errno : -EIO;
        }
        p += n;
        offset += n;
        len -= n;
    }
    return 0;
}
#endif

/* The overlay may allocate or free clusters: forget their mappings. */
static void nand_disk_remap(NANDDisk *d)
{
    if (d->map && ++d->map_generation == 0) {
        memset(d->map, 0, NAND_DISK_MAP_SLOTS * sizeof(*d->map));
        d->map_generation = 1;
    }
}

#ifndef _WIN32
/*
 * Where `offset`'s cluster lives: an entry of d->map, filled from qcow2's mapping of the whole cluster (its cached
 * L2 tables) when it has none; NULL when the cluster is not mapped as one (the caller takes the block layer).
 * Only the mapping is asked for: a precise block status would also look for holes in the host file (qcow2 does
 * when a file holds fewer bytes than its clusters, as an image written sparsely does), a syscall per read that
 * describes part of a cluster.
 */
static const NANDDiskMapping *nand_disk_map(NANDDisk *d, uint64_t offset)
{
    uint64_t cluster = offset / NAND_DISK_CLUSTER;
    NANDDiskMapping *m = &d->map[cluster % NAND_DISK_MAP_SLOTS];
    int64_t pnum = 0, map = 0;
    BlockDriverState *file = NULL;
    int status;

    if (m->kind && m->generation == d->map_generation && m->cluster == cluster) {
        return m;
    }
    status = bdrv_common_block_status_above(blk_bs(d->blk), NULL, false,
                                            BDRV_WANT_OFFSET_VALID | BDRV_WANT_ALLOCATED,
                                            cluster * NAND_DISK_CLUSTER, NAND_DISK_CLUSTER, &pnum, &map, &file, NULL);
    if (status < 0 || pnum < NAND_DISK_CLUSTER) {
        return NULL;
    }
    if (status & BDRV_BLOCK_ZERO) {
        m->kind = NAND_DISK_ZERO;
    } else if ((status & BDRV_BLOCK_DATA) && (status & BDRV_BLOCK_OFFSET_VALID) &&
               (file == d->top_file || file == d->base_file)) {
        m->kind = file == d->top_file ? NAND_DISK_TOP : NAND_DISK_BASE;
        m->host = map;
    } else if (!(status & (BDRV_BLOCK_DATA | BDRV_BLOCK_ALLOCATED))) {
        m->kind = NAND_DISK_ZERO;       /* allocated in no layer: the system image has no backing */
    } else {
        return NULL;
    }
    m->cluster = cluster;
    m->generation = d->map_generation;
    return m;
}
#endif

/*
 * Reads pread the host file holding the data, the overlay's or the system image's, where qcow2's mapping puts it.
 * Through the block layer every page was a coroutine and a thread-pool round trip (~45 us on the desktop, with
 * the BQL held).
 */
int nand_disk_read(NANDDisk *d, uint64_t offset, uint32_t len, void *buf)
{
#ifndef _WIN32
    uint8_t *p = buf;

    while (len && d->map) {
        const NANDDiskMapping *m = nand_disk_map(d, offset);
        uint32_t n = MIN(len, NAND_DISK_CLUSTER - offset % NAND_DISK_CLUSTER);

        if (!m) {
            break;
        }
        if (m->kind == NAND_DISK_ZERO) {
            memset(p, 0, n);
        } else {
            int result = nand_disk_pio(m->kind == NAND_DISK_TOP ? d->top_fd : d->base_fd, p, n,
                                       m->host + offset % NAND_DISK_CLUSTER, false);

            if (result < 0) {
                return result;
            }
        }
        p += n;
        offset += n;
        len -= n;
    }
    if (!len) {
        return 0;
    }
    buf = p;
#endif
    return blk_pread(d->blk, offset, len, buf, 0);
}

/*
 * A program into an overlay cluster it already holds is a pwrite (qcow2's metadata does not change); one that
 * allocates goes through qcow2, its metadata written out once the burst ends (a flush, which NO_FLUSH keeps from
 * syncing).
 */
int nand_disk_write(NANDDisk *d, uint64_t offset, uint32_t len, const void *buf)
{
    int result;

#ifndef _WIN32
    if (d->map && offset % NAND_DISK_CLUSTER + len <= NAND_DISK_CLUSTER) {
        const NANDDiskMapping *m = nand_disk_map(d, offset);

        if (m && m->kind == NAND_DISK_TOP) {
            return nand_disk_pio(d->top_fd, (void *)buf, len, m->host + offset % NAND_DISK_CLUSTER, true);
        }
    }
    nand_disk_remap(d);
#endif
    result = blk_pwrite(d->blk, offset, len, buf, 0);
    qemu_bh_schedule(d->flush);
    return result;
}

int nand_disk_erase(NANDDisk *d, unsigned chip, uint32_t block)
{
    int result;

    if (chip >= d->chips || block >= d->blocks_per_chip) {
        return -EINVAL;
    }
    nand_disk_remap(d);
    /* The freed clusters' new mapping reaches the OS before any program can reuse them. */
    result = blk_pwrite_zeroes(d->blk, chip * d->chip_stride + block * d->block_stride, d->block_stride,
                               BDRV_REQ_MAY_UNMAP);
    return result < 0 ? result : blk_flush(d->blk);
}

int nand_disk_sync(NANDDisk *d)
{
    int result;

    qemu_bh_cancel(d->flush);
    result = d->flush_error ? d->flush_error : blk_flush(d->blk);

    if (result >= 0 && d->sync_fd >= 0 && qemu_fdatasync(d->sync_fd) < 0) {
        result = -errno;
    }
    return result;
}
