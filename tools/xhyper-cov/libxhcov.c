/*
 * libxhcov - production QEMU TCG coverage plugin for fuzzing XHyper (AArch64, EL2)
 *
 * Records the SET of distinct translation blocks (TBs) whose start vaddr lies
 * in [text_start, text_end) AND that execute at EL2 (read from the "cpsr"
 * gdb-core register, EL = bits [3:2]). EL1 execution is ignored: EL1 and EL2
 * can share high VAs, so the PSTATE check is mandatory. Coverage is a bitmap,
 * not a trace: a free-running hypervisor re-executes the same blocks ~1e9
 * times, so an append-only execution log is not a usable per-program set.
 *
 * Output: two shm-backed files per vCPU, little-endian, identical layout.
 *   Window (syz-manager read-and-clear): "<shm_prefix>.<vcpu_index>"
 *   Cumulative (never cleared):           "<shm_prefix>_cum.<vcpu_index>"
 *   e.g. shm=/dev/shm/xh0 -> /dev/shm/xh0.0 and /dev/shm/xh0_cum.0.
 * The drainer only opens the window file. The cumulative file is a separate
 * mapping so a SWAP-to-zero of the window words cannot clear it.
 *   Header (4096 bytes), both files:
 *     off 0   u64 magic        = 0x323030564f434858 ("XHCOV002" as LE bytes)
 *     off 8   u64 version      = 2
 *     off 16  u64 cpu_index
 *     off 24  u64 text_start   (parsed text_start; base for index math)
 *     off 32  u64 granularity  = 4 (AArch64 insns are 4 bytes; TB starts
 *                               are 4-aligned)
 *     off 40  u64 nwords       (u64 words in the bitmap)
 *     off 48  u64 reserved[]   (zero-filled to 4096)
 *   Bitmap (offset 4096): nwords u64 words.
 *     Bit k (word k/64, bit k%64) set => the TB whose start vaddr ==
 *     text_start + k*granularity. On the window file that means "covered
 *     since the last clear"; on the cumulative file it means "covered at
 *     any point in this QEMU process".
 *   File size = 4096 + nwords*8.
 *
 * nwords = ceil(((text_end - text_start) / granularity) / 64).
 *
 * The writer only ever ORs bits (never clears), with relaxed atomics on the
 * u64 words, in both files. An external reader atomically SWAPs each window
 * word to 0 (read-and-clear) to take the per-window covered set; it must
 * not touch the cumulative file. No other synchronization. The hot path
 * load-checks a word before the OR, so a block that is already covered
 * costs one relaxed load per bitmap and then nothing.
 *
 * The "cpsr" handle is discovered lazily in vcpu_init (NEVER in
 * qemu_plugin_install: QEMU asserts current_cpu there), and each TB's
 * exception level is classified once and cached on its per-TB userdata
 * (a QEMU TB is EL-specific, so the classification is stable).
 *
 * Two exclusive modes, selected by whether the trace_pc argument is present:
 *
 *   TB-start (default, trace_pc absent). Unchanged: one exec callback per
 *   in-range TB, bit key = TB start vaddr.
 *
 *   SanCov (trace_pc=<hex> present). Used when the guest is a
 *   SanCov-instrumented XHyper, which plants `bl __sanitizer_cov_trace_pc`
 *   at each basic block. syzkaller's coverage report keys on the call's
 *   return address (it subtracts 4 from the reported PC to recover the
 *   call site), so this mode records (call_site_vaddr + 4) and does not
 *   record TB starts. At translate time each in-range instruction is
 *   decoded; an AArch64 BL whose target equals trace_pc gets a per-insn
 *   exec callback. That callback reuses the same EL2 cpsr classification
 *   (cached on the call site; a TB is EL-specific and a BL ends its TB)
 *   and ORs the bit for (call_site + 4) into both bitmaps with the same
 *   idx = (va - text_start) / 4 load-check-then-OR as TB-start mode.
 *   Header layout is identical either way.
 *
 * Plugin args: text_start=<hex>, text_end=<hex>, shm=<prefix>
 *              [,trace_pc=<hex>]
 * Target: QEMU 10.2.1, plugin API v5, aarch64 system emulation.
 */

#include "qemu-plugin.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define XHCOV_MAGIC       0x323030564f434858ULL /* "XHCOV002" as LE bytes */
#define XHCOV_FMT_VERSION 2
#define XHCOV_HDR_BYTES   4096u
#define XHCOV_GRANULARITY 4u /* AArch64 instruction size; TB starts are aligned */
#define XHCOV_MAX_VCPUS   4096u

/* tri-state EL classification cached per TB */
enum {
    XHCOV_EL_UNKNOWN = 0,
    XHCOV_EL_IS_EL2  = 1,
    XHCOV_EL_NOT_EL2 = 2,
};

struct xhcov_header {
    uint64_t magic;
    uint64_t version;
    uint64_t cpu_index;
    uint64_t text_start;
    uint64_t granularity;
    uint64_t nwords;
    uint64_t reserved[(XHCOV_HDR_BYTES - 48) / sizeof(uint64_t)];
};

_Static_assert(sizeof(struct xhcov_header) == XHCOV_HDR_BYTES,
               "xhcov header is one page");
_Static_assert(offsetof(struct xhcov_header, text_start) == 24,
               "text_start at offset 24");
_Static_assert(offsetof(struct xhcov_header, nwords) == 40,
               "nwords at offset 40");

struct xhcov_bitmap {
    void *map;             /* mmap base (header first) */
    size_t map_size;
    uint64_t *words;       /* bitmap, right after the 4096-byte header */
    uint64_t nwords;
};

struct xhcov_tb {
    uint64_t vaddr;     /* TB start vaddr to record */
    uint32_t el_class;  /* XHCOV_EL_*; accessed only via __atomic_* */
    uint32_t pad;
};

static uint64_t g_text_start;
static uint64_t g_text_end;
static uint64_t g_nwords;
static char *g_shm_prefix;

/* trace_pc present => SanCov call-site mode; absent => TB-start mode. */
static int g_sancov_mode;
static uint64_t g_trace_pc;

static struct xhcov_bitmap *g_bitmaps[XHCOV_MAX_VCPUS];
static struct xhcov_bitmap *g_cum_bitmaps[XHCOV_MAX_VCPUS];

static struct qemu_plugin_register *g_cpsr_handle; /* release-published */
static pthread_mutex_t g_disc_lock = PTHREAD_MUTEX_INITIALIZER;

/* debug/diag counters, relaxed atomics, printed once at exit */
static uint64_t g_n_tb_instrumented;
static uint64_t g_n_bits_set;
static uint64_t g_n_cls_not_el2;
static uint64_t g_n_skip_noreg;
static uint64_t g_n_sancov_sites;

static uint64_t le64_from_bytes(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    size_t i;

    if (n > sizeof(v)) {
        n = sizeof(v);
    }
    for (i = 0; i < n; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/*
 * ceil(((text_end - text_start) / granularity) / 64), computed in integers
 * as ceil(span / (granularity * 64)). span is nonzero (start < end).
 */
static int xhcov_compute_nwords(void)
{
    uint64_t span = g_text_end - g_text_start;
    uint64_t denom = (uint64_t)XHCOV_GRANULARITY * 64u;
    uint64_t nwords = span / denom + (span % denom != 0);

    /* idx < nwords*64 must not wrap, and the file must fit in size_t. */
    if (nwords == 0 || nwords > (UINT64_MAX >> 6) ||
        nwords > (SIZE_MAX - XHCOV_HDR_BYTES) / sizeof(uint64_t)) {
        fprintf(stderr, "[xhcov] text range does not fit a coverage bitmap\n");
        return -1;
    }
    g_nwords = nwords;
    return 0;
}

/*
 * Discover the "cpsr" register handle. Must run in vCPU context (vcpu_init
 * or a tb-exec callback), never from qemu_plugin_install(). Returns 1 when
 * the handle is available.
 */
static int xhcov_ensure_cpsr(void)
{
    if (__atomic_load_n(&g_cpsr_handle, __ATOMIC_ACQUIRE) != NULL) {
        return 1;
    }
    pthread_mutex_lock(&g_disc_lock);
    if (__atomic_load_n(&g_cpsr_handle, __ATOMIC_RELAXED) == NULL) {
        GArray *regs = qemu_plugin_get_registers();

        if (regs != NULL) {
            guint i;

            for (i = 0; i < regs->len; i++) {
                qemu_plugin_reg_descriptor *rd =
                    &g_array_index(regs, qemu_plugin_reg_descriptor, i);

                if (rd->name != NULL && strcmp(rd->name, "cpsr") == 0) {
                    __atomic_store_n(&g_cpsr_handle, rd->handle,
                                     __ATOMIC_RELEASE);
                    break;
                }
            }
            /* caller frees the array, not the const strings */
            g_array_free(regs, TRUE);
        }
    }
    pthread_mutex_unlock(&g_disc_lock);
    return __atomic_load_n(&g_cpsr_handle, __ATOMIC_ACQUIRE) != NULL;
}

/* Read PSTATE via "cpsr" and return EL (bits [3:2]), or -1 on failure. */
static int xhcov_current_el(void)
{
    struct qemu_plugin_register *h =
        __atomic_load_n(&g_cpsr_handle, __ATOMIC_ACQUIRE);
    GByteArray *buf;
    uint64_t pstate;
    int n;

    if (h == NULL) {
        return -1;
    }
    buf = g_byte_array_new();
    n = qemu_plugin_read_register(h, buf);
    if (n < 0 || buf->len == 0) {
        g_byte_array_unref(buf);
        return -1;
    }
    /* aarch64 guest is little-endian */
    pstate = le64_from_bytes(buf->data, buf->len);
    g_byte_array_unref(buf);
    return (int)((pstate >> 2) & 3);
}

/*
 * Create one bitmap file (window or cumulative). Same header, same size.
 * Returns NULL on failure after logging; the caller leaves that slot empty.
 */
static struct xhcov_bitmap *xhcov_create_bitmap(const char *path,
                                               unsigned int vcpu_index)
{
    struct xhcov_bitmap *bm;
    struct xhcov_header *h;
    size_t sz;
    void *map;
    int fd;

    sz = (size_t)XHCOV_HDR_BYTES + (size_t)g_nwords * sizeof(uint64_t);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "[xhcov] open(%s): %s\n", path, strerror(errno));
        return NULL;
    }
    if (ftruncate(fd, (off_t)sz) != 0) {
        fprintf(stderr, "[xhcov] ftruncate(%s): %s\n", path, strerror(errno));
        close(fd);
        return NULL;
    }
    map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "[xhcov] mmap(%s): %s\n", path, strerror(errno));
        return NULL;
    }
    memset(map, 0, sz);

    h = (struct xhcov_header *)map;
    h->magic = XHCOV_MAGIC;
    h->version = XHCOV_FMT_VERSION;
    h->cpu_index = vcpu_index;
    h->text_start = g_text_start;
    h->granularity = XHCOV_GRANULARITY;
    h->nwords = g_nwords;

    bm = g_new0(struct xhcov_bitmap, 1);
    bm->map = map;
    bm->map_size = sz;
    bm->words = (uint64_t *)((char *)map + XHCOV_HDR_BYTES);
    bm->nwords = g_nwords;
    return bm;
}

static void xhcov_setup_one(struct xhcov_bitmap **slot, const char *path,
                            unsigned int vcpu_index)
{
    struct xhcov_bitmap *bm;

    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE) != NULL) {
        return;
    }
    bm = xhcov_create_bitmap(path, vcpu_index);
    if (bm != NULL) {
        __atomic_store_n(slot, bm, __ATOMIC_RELEASE);
    }
}

static void xhcov_setup_bitmap(unsigned int vcpu_index)
{
    char path[PATH_MAX];

    if (vcpu_index >= XHCOV_MAX_VCPUS) {
        return;
    }
    if (g_shm_prefix == NULL || g_nwords == 0) {
        return;
    }
    if (snprintf(path, sizeof(path), "%s.%u", g_shm_prefix, vcpu_index)
        >= (int)sizeof(path)) {
        fprintf(stderr, "[xhcov] shm path too long for vcpu %u\n", vcpu_index);
    } else {
        xhcov_setup_one(&g_bitmaps[vcpu_index], path, vcpu_index);
    }
    if (snprintf(path, sizeof(path), "%s_cum.%u", g_shm_prefix, vcpu_index)
        >= (int)sizeof(path)) {
        fprintf(stderr, "[xhcov] cumulative shm path too long for vcpu %u\n",
                vcpu_index);
    } else {
        xhcov_setup_one(&g_cum_bitmaps[vcpu_index], path, vcpu_index);
    }
}

static void xhcov_vcpu_init_cb(qemu_plugin_id_t id, unsigned int vcpu_index)
{
    (void)id;
    /* safe vCPU-context site for lazy register discovery */
    (void)xhcov_ensure_cpsr();
    xhcov_setup_bitmap(vcpu_index);
}

/*
 * Hot path: runs on every execution of an instrumented TB, in the vCPU
 * thread. Bounded, non-blocking: only atomic loads/stores, no malloc, no
 * syscalls, no locks (the mutex in xhcov_ensure_cpsr is only ever taken
 * while g_cpsr_handle is still NULL, i.e. during startup).
 *
 * Once a bit is set, later executions of that TB load the word, see the
 * bit, and skip the OR. The reader may SWAP a window word to 0 between
 * windows; the next execution ORs that window bit again. The cumulative
 * bit is ORed the same way and is never cleared, so after the first hit it
 * stays set for the whole QEMU process.
 */
static void xhcov_tb_exec_cb(unsigned int vcpu_index, void *userdata)
{
    struct xhcov_tb *t = (struct xhcov_tb *)userdata;
    uint32_t cls;
    struct xhcov_bitmap *bm;
    struct xhcov_bitmap *cbm;
    uint64_t idx, w, b, mask;

    cls = __atomic_load_n(&t->el_class, __ATOMIC_ACQUIRE);
    if (cls == XHCOV_EL_NOT_EL2) {
        __atomic_fetch_add(&g_n_cls_not_el2, 1, __ATOMIC_RELAXED);
        return;
    }

    if (cls == XHCOV_EL_UNKNOWN) {
        int el;

        if (!xhcov_ensure_cpsr()) {
            __atomic_fetch_add(&g_n_skip_noreg, 1, __ATOMIC_RELAXED);
            return;
        }
        el = xhcov_current_el();
        if (el < 0) {
            __atomic_fetch_add(&g_n_skip_noreg, 1, __ATOMIC_RELAXED);
            return;
        }
        cls = (el == 2) ? XHCOV_EL_IS_EL2 : XHCOV_EL_NOT_EL2;
        /* a QEMU TB is EL-specific: classify once, cache for good */
        __atomic_store_n(&t->el_class, cls, __ATOMIC_RELEASE);
        if (cls != XHCOV_EL_IS_EL2) {
            __atomic_fetch_add(&g_n_cls_not_el2, 1, __ATOMIC_RELAXED);
            return;
        }
    }

    bm = __atomic_load_n(&g_bitmaps[vcpu_index], __ATOMIC_ACQUIRE);
    cbm = __atomic_load_n(&g_cum_bitmaps[vcpu_index], __ATOMIC_ACQUIRE);
    if (bm == NULL && cbm == NULL) {
        return;
    }
    idx = (t->vaddr - g_text_start) / XHCOV_GRANULARITY;
    w = idx >> 6;
    b = idx & 63;
    mask = 1ULL << b;
    if (bm != NULL && idx < bm->nwords * 64 &&
        (__atomic_load_n(&bm->words[w], __ATOMIC_RELAXED) & mask) == 0) {
        __atomic_fetch_or(&bm->words[w], mask, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_n_bits_set, 1, __ATOMIC_RELAXED);
    }
    if (cbm != NULL && idx < cbm->nwords * 64 &&
        (__atomic_load_n(&cbm->words[w], __ATOMIC_RELAXED) & mask) == 0) {
        __atomic_fetch_or(&cbm->words[w], mask, __ATOMIC_RELAXED);
    }
}

/*
 * SanCov call site. vaddr is the value recorded in the bitmap (the BL's
 * return address, call_site + 4). el_class is the same tri-state cache the
 * TB-start path keeps on its userdata: one BL ends the TB, and that TB is
 * EL-specific, so the first execution classifies every later one.
 */
struct xhcov_site {
    uint64_t vaddr;
    uint32_t el_class;
    uint32_t pad;
};

/*
 * AArch64 BL: bits[31:26] == 0b100101, target = PC + SignExtend(imm26 << 2).
 * Sign-extend via bit 31: (imm26 << 6) moves bit 25 onto bit 31 of a 32-bit
 * word; an arithmetic >> 4 then yields sign_extend_26(imm26) << 2. The
 * int32_t cast is required: widening the uint32 shift to int64 first
 * zero-extends, and almost every site branches backward to trace_pc.
 */
static int xhcov_is_trace_pc_bl(uint64_t iva, uint32_t enc)
{
    uint32_t imm26;
    int64_t off;
    uint64_t target;

    if ((enc & 0xFC000000u) != 0x94000000u) {
        return 0;
    }
    imm26 = enc & 0x03FFFFFFu;
    off = ((int64_t)(int32_t)(imm26 << 6)) >> 4;
    target = iva + (uint64_t)off;
    return target == g_trace_pc;
}

/* Same EL2 gate and bitmap OR as xhcov_tb_exec_cb, keyed by site->vaddr. */
static void xhcov_sancov_exec_cb(unsigned int vcpu_index, void *userdata)
{
    struct xhcov_site *s = (struct xhcov_site *)userdata;
    uint32_t cls;
    struct xhcov_bitmap *bm;
    struct xhcov_bitmap *cbm;
    uint64_t idx, w, b, mask;

    cls = __atomic_load_n(&s->el_class, __ATOMIC_ACQUIRE);
    if (cls == XHCOV_EL_NOT_EL2) {
        __atomic_fetch_add(&g_n_cls_not_el2, 1, __ATOMIC_RELAXED);
        return;
    }

    if (cls == XHCOV_EL_UNKNOWN) {
        int el;

        if (!xhcov_ensure_cpsr()) {
            __atomic_fetch_add(&g_n_skip_noreg, 1, __ATOMIC_RELAXED);
            return;
        }
        el = xhcov_current_el();
        if (el < 0) {
            __atomic_fetch_add(&g_n_skip_noreg, 1, __ATOMIC_RELAXED);
            return;
        }
        cls = (el == 2) ? XHCOV_EL_IS_EL2 : XHCOV_EL_NOT_EL2;
        __atomic_store_n(&s->el_class, cls, __ATOMIC_RELEASE);
        if (cls != XHCOV_EL_IS_EL2) {
            __atomic_fetch_add(&g_n_cls_not_el2, 1, __ATOMIC_RELAXED);
            return;
        }
    }

    bm = __atomic_load_n(&g_bitmaps[vcpu_index], __ATOMIC_ACQUIRE);
    cbm = __atomic_load_n(&g_cum_bitmaps[vcpu_index], __ATOMIC_ACQUIRE);
    if (bm == NULL && cbm == NULL) {
        return;
    }
    idx = (s->vaddr - g_text_start) / XHCOV_GRANULARITY;
    w = idx >> 6;
    b = idx & 63;
    mask = 1ULL << b;
    if (bm != NULL && idx < bm->nwords * 64 &&
        (__atomic_load_n(&bm->words[w], __ATOMIC_RELAXED) & mask) == 0) {
        __atomic_fetch_or(&bm->words[w], mask, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_n_bits_set, 1, __ATOMIC_RELAXED);
    }
    if (cbm != NULL && idx < cbm->nwords * 64 &&
        (__atomic_load_n(&cbm->words[w], __ATOMIC_RELAXED) & mask) == 0) {
        __atomic_fetch_or(&cbm->words[w], mask, __ATOMIC_RELAXED);
    }
}

/*
 * Translate-time scan. Only instructions inside [text_start, text_end) are
 * candidates. A matching BL is instrumented with an exec callback; TB-start
 * callbacks are not registered in this mode.
 */
static void xhcov_sancov_tb_trans(struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    size_t i;
    uint64_t va0;

    if (n == 0) {
        return;
    }
    /*
     * AArch64 TBs are a linear run of 4-byte insns. Skip a block whose
     * whole span misses [text_start, text_end); the per-insn check below
     * is what actually decides.
     */
    va0 = qemu_plugin_tb_vaddr(tb);
    if (va0 >= g_text_end ||
        va0 + (uint64_t)(n - 1) * XHCOV_GRANULARITY < g_text_start) {
        return;
    }

    for (i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t iva = qemu_plugin_insn_vaddr(insn);
        uint32_t enc = 0;
        struct xhcov_site *s;

        if (iva < g_text_start || iva >= g_text_end) {
            continue;
        }
        if (qemu_plugin_insn_data(insn, &enc, 4) != 4) {
            continue;
        }
        if (!xhcov_is_trace_pc_bl(iva, enc)) {
            continue;
        }
        s = g_new0(struct xhcov_site, 1);
        /* syzkaller keys coverage on the return address, i.e. the BL + 4. */
        s->vaddr = iva + 4;
        s->el_class = XHCOV_EL_UNKNOWN;
        __atomic_fetch_add(&g_n_sancov_sites, 1, __ATOMIC_RELAXED);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, xhcov_sancov_exec_cb,
                                               QEMU_PLUGIN_CB_R_REGS, s);
    }
}

static void xhcov_tb_trans_cb(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    uint64_t vaddr = qemu_plugin_tb_vaddr(tb);
    struct xhcov_tb *t;

    (void)id;
    if (g_sancov_mode) {
        xhcov_sancov_tb_trans(tb);
        return;
    }
    if (vaddr < g_text_start || vaddr >= g_text_end) {
        return;
    }
    t = g_new0(struct xhcov_tb, 1);
    t->vaddr = vaddr;
    t->el_class = XHCOV_EL_UNKNOWN;
    __atomic_fetch_add(&g_n_tb_instrumented, 1, __ATOMIC_RELAXED);
    /*
     * t is intentionally never freed: TB count in the text window is bounded
     * and small; freeing on flush would risk races for no practical gain.
     */
    qemu_plugin_register_vcpu_tb_exec_cb(tb, xhcov_tb_exec_cb,
                                         QEMU_PLUGIN_CB_R_REGS, t);
}

static void xhcov_atexit_cb(qemu_plugin_id_t id, void *userdata)
{
    unsigned int i;

    (void)id;
    (void)userdata;
    for (i = 0; i < XHCOV_MAX_VCPUS; i++) {
        struct xhcov_bitmap *bm =
            __atomic_load_n(&g_bitmaps[i], __ATOMIC_ACQUIRE);
        struct xhcov_bitmap *cbm =
            __atomic_load_n(&g_cum_bitmaps[i], __ATOMIC_ACQUIRE);

        if (bm != NULL && bm->map != NULL) {
            (void)msync(bm->map, bm->map_size, MS_SYNC);
        }
        if (cbm != NULL && cbm->map != NULL) {
            (void)msync(cbm->map, cbm->map_size, MS_SYNC);
        }
    }
    fprintf(stderr,
            "[xhcov] tbs_instrumented=%" PRIu64 " el2_bits_set=%" PRIu64
            " execs_not_el2=%" PRIu64 " execs_skipped_noreg=%" PRIu64 "\n",
            __atomic_load_n(&g_n_tb_instrumented, __ATOMIC_RELAXED),
            __atomic_load_n(&g_n_bits_set, __ATOMIC_RELAXED),
            __atomic_load_n(&g_n_cls_not_el2, __ATOMIC_RELAXED),
            __atomic_load_n(&g_n_skip_noreg, __ATOMIC_RELAXED));
    if (g_sancov_mode) {
        fprintf(stderr,
                "[xhcov] sancov trace_pc=0x%" PRIx64
                " call_sites_instrumented=%" PRIu64 "\n",
                g_trace_pc,
                __atomic_load_n(&g_n_sancov_sites, __ATOMIC_RELAXED));
    }
}

static int parse_hex(const char *s, uint64_t *out)
{
    guint64 v;
    char *end;

    errno = 0;
    v = g_ascii_strtoull(s, &end, 16);
    if (errno != 0 || end == s || *end != '\0') {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    int i;
    int have_start = 0, have_end = 0, have_shm = 0, have_trace = 0;

    /*
     * NOTE: never call qemu_plugin_get_registers()/register reads here -
     * QEMU asserts (current_cpu) and aborts. Discovery is lazy, in vCPU
     * context (see xhcov_ensure_cpsr).
     */
    if (!info->system_emulation) {
        fprintf(stderr, "[xhcov] requires system emulation\n");
        return -1;
    }

    for (i = 0; i < argc; i++) {
        char *opt = argv[i];

        if (g_str_has_prefix(opt, "text_start=")) {
            have_start = parse_hex(opt + strlen("text_start="), &g_text_start);
        } else if (g_str_has_prefix(opt, "text_end=")) {
            have_end = parse_hex(opt + strlen("text_end="), &g_text_end);
        } else if (g_str_has_prefix(opt, "shm=")) {
            g_shm_prefix = g_strdup(opt + strlen("shm="));
            have_shm = g_shm_prefix[0] != '\0';
        } else if (g_str_has_prefix(opt, "trace_pc=")) {
            have_trace = parse_hex(opt + strlen("trace_pc="), &g_trace_pc);
            if (!have_trace) {
                fprintf(stderr, "[xhcov] bad trace_pc: %s\n", opt);
                return -1;
            }
        } else {
            fprintf(stderr, "[xhcov] unknown option: %s\n", opt);
            return -1;
        }
    }
    if (!have_start || !have_end || !have_shm) {
        fprintf(stderr,
                "[xhcov] usage: text_start=<hex>,text_end=<hex>,shm=<prefix>"
                "[,trace_pc=<hex>]\n");
        return -1;
    }
    g_sancov_mode = have_trace;
    if (g_text_start >= g_text_end) {
        fprintf(stderr, "[xhcov] text_start must be < text_end\n");
        return -1;
    }
    if (xhcov_compute_nwords() != 0) {
        return -1;
    }

    qemu_plugin_register_vcpu_init_cb(id, xhcov_vcpu_init_cb);
    qemu_plugin_register_vcpu_tb_trans_cb(id, xhcov_tb_trans_cb);
    qemu_plugin_register_atexit_cb(id, xhcov_atexit_cb, NULL);
    return 0;
}
