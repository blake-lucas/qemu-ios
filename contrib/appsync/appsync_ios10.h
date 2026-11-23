// Light Touch AppSync, iOS 10 (Darwin 16) part. Included by appsync.c.
//
// iOS 10's dyld ignores DYLD_INSERT_LIBRARIES in every code-signing-enforced process without get-task-allow, so the
// launchd job's insertion never reaches installd there, and __interpose needs an inserted library. FirmwareKit gives
// 14G61's installd and amfid a weak LC_LOAD_DYLIB of this library instead (both are re-registered in the kernel's
// trust table), and this constructor points those two programs' own MISValidateSignatureAndCopyInfo imports at
// the hook. Nothing else is rebound and nothing runs on older iOS (whose dyld honors the insertion).
//
// iOS 10's kernel also honors no AMFI boot argument: a user app's executable runs only once amfid vouches for its
// code directory hash, and only while every page matches that directory. So:
// - installd: before validating an app, the app bundle's 32-bit ARM Mach-O pages are re-hashed in place into their
//   existing code directories (a decrypted store copy has a changed first page, cryptid). Signatures, special
//   slots and entitlements are kept; a file without a code signature is left alone.
// - amfid: where libmis refuses a file, the hook answers with that file's code directory hash (the one the kernel
//   picked: SHA-256 over SHA-1), which is what amfid hands back to the kernel.

extern const void *_dyld_get_image_header(unsigned int);
extern long _dyld_get_image_vmaddr_slide(unsigned int);
extern int mprotect(void *, unsigned long, int);
extern long pwrite(int, const void *, unsigned long, long long);
#include <dirent.h> // (legacy builds have it already, with readdir remapped; this code never runs there)
extern unsigned char *CC_SHA1(const void *, unsigned int, unsigned char *);
extern unsigned char *CC_SHA256(const void *, unsigned int, unsigned char *);
extern unsigned long strlen(const char *);
extern Boolean CFNumberGetValue(CFTypeRef, CFIndex, void *);
extern unsigned long CFNumberGetTypeID(void);
extern void *memcpy(void *, const void *, unsigned long);
// To the system log through CFLog (os_log underneath): a sandboxed daemon's syslog(3) may reach nothing.
extern void CFLog(int, CFStringRef, ...);
static void as_log(const char *text) {
    static CFStringRef format;
    if (!format) format = CFStringCreateWithCString(0, "LightTouch AppSync: %@", 0x08000100);
    CFStringRef message = CFStringCreateWithCString(0, text, 0x08000100);
    if (format && message) CFLog(4 /* kCFLogLevelWarning */, format, message);
    if (message) CFRelease((CFTypeRef)message);
}
#define AS_LOG(...) do { char as_line[512]; snprintf(as_line, sizeof as_line, __VA_ARGS__); as_log(as_line); } while (0)

// iOS 10 or later: CoreFoundation 1348 came with 10.0. (Not sysctl: a sandboxed installd may not read it.)
static int as_ios10(void) {
    const double *cf = dlsym(RTLD_DEFAULT, "kCFCoreFoundationVersionNumber");
    return cf && *cf >= 1348.0;
}

static int in_amfid(void) {
    const char *p = getprogname();
    return p && !strcmp(p, "amfid");
}

static unsigned le32(const unsigned char *p) {
    return (unsigned)p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

// ---- rebinding the main executable's own imports (no interposition without insertion) ----------------------
static int as_rebind(const char *symbol, const void *replacement) {
    int bound = 0;
    const unsigned char *mh = _dyld_get_image_header(0);
    long slide = _dyld_get_image_vmaddr_slide(0);
    if (!mh || le32(mh) != 0xfeedface) return -1;
    unsigned ncmds = le32(mh + 16);
    const unsigned char *lc = mh + 28;
    unsigned link_vm = 0, link_off = 0, symoff = 0, nsyms = 0, stroff = 0, indoff = 0, nind = 0;
    int have_link = 0;
    for (unsigned i = 0; i < ncmds; i++) {
        unsigned cmd = le32(lc), size = le32(lc + 4);
        if (size < 8) return -1;
        if (cmd == 1 && !memcmp(lc + 8, "__LINKEDIT", 11)) { link_vm = le32(lc + 24); link_off = le32(lc + 32); have_link = 1; }
        if (cmd == 2) { symoff = le32(lc + 8); nsyms = le32(lc + 12); stroff = le32(lc + 16); }
        if (cmd == 0xb) { indoff = le32(lc + 56); nind = le32(lc + 60); }
        lc += size;
    }
    if (!have_link || !nsyms || !nind) return -1;
    const unsigned char *linkedit = (const unsigned char *)(slide + link_vm - link_off);
    const unsigned char *syms = linkedit + symoff;
    const char *strings = (const char *)linkedit + stroff;
    const unsigned char *indirect = linkedit + indoff;
    lc = mh + 28;
    for (unsigned i = 0; i < ncmds; i++) {
        unsigned cmd = le32(lc), size = le32(lc + 4);
        if (cmd == 1) {
            unsigned nsects = le32(lc + 48);
            for (unsigned s = 0; s < nsects && 56 + 68 * (s + 1) <= size; s++) {
                const unsigned char *sect = lc + 56 + 68 * s;
                unsigned type = le32(sect + 56) & 0xff;
                if (type != 6 && type != 7) continue; // non-lazy and lazy symbol pointers
                unsigned addr = le32(sect + 32), bytes = le32(sect + 36), first = le32(sect + 60);
                for (unsigned j = 0; j < bytes / 4 && first + j < nind; j++) {
                    unsigned index = le32(indirect + 4 * (first + j));
                    if (index & 0xc0000000u || index >= nsyms) continue;
                    if (strcmp(strings + le32(syms + 12 * index), symbol)) continue;
                    void **slot = (void **)(slide + addr) + j;
                    unsigned long page = (unsigned long)slot & ~4095ul;
                    mprotect((void *)page, 4096, 3);
                    *slot = (void *)replacement;
                    bound++;
                }
            }
        }
        lc += size;
    }
    return bound;
}

// ---- code directories ----------------------------------------------------------------------------------------
// One 32-bit ARM Mach-O at `base` in `fd`: the superblob's file offset and size, or 0.
static int as_signature(int fd, long long base, unsigned *sig_off, unsigned *sig_size) {
    unsigned char *h = malloc(HEADER_BYTES);
    if (!h) return 0;
    long n = pread(fd, h, HEADER_BYTES, base);
    int found = 0;
    if (n >= 28 && le32(h) == 0xfeedface && le32(h + 4) == 12) {
        unsigned ncmds = le32(h + 16), off = 28;
        for (unsigned i = 0; i < ncmds && off + 16 <= (unsigned)n; i++) {
            unsigned cmd = le32(h + off), size = le32(h + off + 4);
            if (cmd == 0x1d) { *sig_off = le32(h + off + 8); *sig_size = le32(h + off + 12); found = 1; break; }
            if (size < 8 || size > (unsigned)n - off) break;
            off += size;
        }
    }
    free(h);
    return found && *sig_size >= 12 && *sig_size < (1u << 24);
}

// The code directories of the superblob `b` (length `len`): calls `each(cd, cd_len, ctx)` for every one.
typedef void (*as_cd_fn)(unsigned char *cd, unsigned cd_len, void *ctx);
static void as_each_directory(unsigned char *b, unsigned len, as_cd_fn each, void *ctx) {
    if (len < 12 || be32(b) != 0xfade0cc0) return;
    unsigned count = be32(b + 8);
    for (unsigned i = 0; i < count && 12 + 8 * (i + 1) <= len; i++) {
        unsigned type = be32(b + 12 + 8 * i), o = be32(b + 16 + 8 * i);
        if (type != 0 && (type < 0x1000 || type > 0x1004)) continue;
        if (o > len - 44 || be32(b + o) != 0xfade0c02) continue;
        unsigned cd_len = be32(b + o + 4);
        if (cd_len < 44 || cd_len > len - o) continue;
        each(b + o, cd_len, ctx);
    }
}

static int as_digest(unsigned type, const void *bytes, unsigned n, unsigned char out[32]) {
    if (type == 1) { CC_SHA1(bytes, n, out); return 20; }
    if (type == 2 || type == 3) { CC_SHA256(bytes, n, out); return type == 2 ? 32 : 20; }
    return 0;
}

// ---- installd: re-hash pages ---------------------------------------------------------------------------------
struct as_rehash { int fd; long long base; unsigned sig_off; unsigned char *page; int changed; };

static void as_rehash_directory(unsigned char *cd, unsigned cd_len, void *ctx) {
    struct as_rehash *r = ctx;
    unsigned hashes = be32(cd + 16), slots = be32(cd + 28), limit = be32(cd + 32);
    unsigned width = cd[36], type = cd[37], shift = cd[39];
    if (shift < 9 || shift > 16 || !width || width > 32 || hashes > cd_len || slots > (cd_len - hashes) / width)
        return;
    unsigned page = 1u << shift;
    if (limit > r->sig_off || (limit + page - 1) / page != slots) return;
    for (unsigned i = 0; i < slots; i++) {
        unsigned lo = i * page, n = limit - lo < page ? limit - lo : page;
        unsigned char digest[32];
        if (pread(r->fd, r->page, n, r->base + lo) != (long)n) return;
        if (as_digest(type, r->page, n, digest) < (int)width) return;
        if (memcmp(cd + hashes + i * width, digest, width)) {
            memcpy(cd + hashes + i * width, digest, width);
            r->changed = 1;
        }
    }
}

static void as_rehash_slice(int fd, long long base) {
    struct as_rehash r = { fd, base, 0, 0, 0 };
    unsigned size = 0;
    if (!as_signature(fd, base, &r.sig_off, &size)) return;
    unsigned char *b = malloc(size);
    r.page = malloc(1u << 16);
    if (b && r.page && pread(fd, b, size, base + r.sig_off) == (long)size) {
        as_each_directory(b, size, as_rehash_directory, &r);
        if (r.changed) pwrite(fd, b, size, base + r.sig_off);
    }
    free(b);
    free(r.page);
}

static void as_rehash_file(const char *file) {
    int fd = open(file, 2); // O_RDWR
    if (fd < 0) return;
    unsigned char h[8 + 20 * 8];
    long n = pread(fd, h, sizeof h, 0);
    if (n >= 8 && be32(h) == 0xcafebabe) {
        unsigned count = be32(h + 4);
        for (unsigned i = 0; i < count && 8 + 20 * (i + 1) <= (unsigned)n; i++)
            if (be32(h + 8 + 20 * i) == 12) as_rehash_slice(fd, be32(h + 8 + 20 * i + 8));
    } else if (n >= 8 && le32(h) == 0xfeedface && le32(h + 4) == 12) {
        as_rehash_slice(fd, 0);
    }
    close(fd);
}

static void as_rehash_tree(char *path, unsigned len, unsigned cap, int depth) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *name = e->d_name;
        unsigned type = e->d_type, n = strlen(name);
        if (!strcmp(name, ".") || !strcmp(name, "..") || len + 1 + n + 1 > cap) continue;
        path[len] = '/';
        memcpy(path + len + 1, name, n + 1);
        if (type == DT_DIR && depth < 12) as_rehash_tree(path, len + 1 + n, cap, depth + 1);
        else if (type == DT_REG) as_rehash_file(path);
        path[len] = 0;
    }
    closedir(dir);
}

// The outermost "*.app" directory of `file` (an executable or a bundle path), re-hashed once per call. Only an app
// installd is installing: one in its staging folder (it validates installed and built-in apps too).
extern char *strstr(const char *, const char *);
static void as_rehash_bundle(CFStringRef path) {
    char file[1024];
    if (!path || CFGetTypeID(path) != CFStringGetTypeID() ||
        !CFStringGetCString(path, file, sizeof file, 0x08000100)) return;
    if (!strstr(file, "/com.apple.mobile.installd.staging/")) return;
    unsigned end = 0;
    for (unsigned i = 0; file[i]; i++) {
        if ((file[i + 1] == '/' || !file[i + 1]) && i >= 4 && !memcmp(file + i - 3, ".app", 4)) { end = i + 1; break; }
    }
    if (!end) return;
    file[end] = 0;
    as_rehash_tree(file, end, sizeof file, 0);
}

// ---- amfid: the code directory hash of a file libmis refused --------------------------------------------------
struct as_best { unsigned rank; unsigned char hash[20]; };

static void as_rank_directory(unsigned char *cd, unsigned cd_len, void *ctx) {
    struct as_best *best = ctx;
    unsigned type = cd[37], rank = type == 2 ? 3 : type == 3 ? 2 : type == 1 ? 1 : 0;
    unsigned char digest[32];
    if (rank <= best->rank || !as_digest(type, cd, cd_len, digest)) return;
    best->rank = rank;
    memcpy(best->hash, digest, 20);
}

static int as_cdhash(CFStringRef path, CFDictionaryRef options, unsigned char out[20]) {
    char file[1024];
    if (!path || CFGetTypeID(path) != CFStringGetTypeID() ||
        !CFStringGetCString(path, file, sizeof file, 0x08000100)) return 0;
    long long base = 0;
    const CFStringRef *offset_key = dlsym(RTLD_DEFAULT, "kMISValidationOptionUniversalFileOffset");
    if (options && offset_key && CFGetTypeID(options) == CFDictionaryGetTypeID()) {
        CFTypeRef v = CFDictionaryGetValue(options, *offset_key);
        if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, 4 /* kCFNumberSInt64Type */, &base);
    }
    int fd = open(file, 0);
    if (fd < 0) return 0;
    struct as_best best = { 0, { 0 } };
    unsigned off = 0, size = 0;
    if (as_signature(fd, base, &off, &size)) {
        unsigned char *b = malloc(size);
        if (b && pread(fd, b, size, base + off) == (long)size) as_each_directory(b, size, as_rank_directory, &best);
        free(b);
    }
    close(fd);
    if (!best.rank) return 0;
    memcpy(out, best.hash, 20);
    return 1;
}

static int as_ios10_amfid(void *path, void *options, CFDictionaryRef *info, int result) {
    unsigned char hash[20];
    const CFStringRef *key = dlsym(RTLD_DEFAULT, "kMISValidationInfoCdHash");
    if (!result) return result;
    char file[256] = "";
    if (path) CFStringGetCString((CFStringRef)path, file, sizeof file, 0x08000100);
    if (!info || !key || !as_cdhash((CFStringRef)path, (CFDictionaryRef)options, hash)) {
        AS_LOG("amfid: %s refused (%#x), no code directory to vouch for", file, result);
        return result;
    }
    AS_LOG("amfid: %s refused (%#x); vouching for cdhash %02x%02x%02x%02x...", file, result, hash[0], hash[1], hash[2], hash[3]);
    if (*info) { CFRelease((CFTypeRef)*info); *info = 0; }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDataRef cd = CFDataCreate(kCFAllocatorDefault, hash, 20);
    if (!d || !cd) {
        if (d) CFRelease((CFTypeRef)d);
        if (cd) CFRelease((CFTypeRef)cd);
        return result;
    }
    CFDictionarySetValue(d, *key, cd);
    CFRelease((CFTypeRef)cd);
    *info = d;
    return 0;
}
