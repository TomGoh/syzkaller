/*
 * xhcov-reader - host-side drain tool for the libxhcov QEMU TCG coverage
 * plugin's per-vCPU shared-memory rings.
 *
 * Each vCPU produces "<shm_prefix>.<idx>": a 4096-byte header (struct
 * xhcov_header below, byte-identical to libxhcov.c) followed by a ring of
 * "capacity" uint64 TB start addresses. Record N lives in slot N % capacity.
 * The ring wraps and overwrites the OLDEST records, so the reader must track
 * how far it has drained. The monotonic write_cursor is release-published by
 * the plugin and read here with an acquire load; the per-vCPU last-drained
 * cursor is persisted in a state file (default "<prefix>.state") so each
 * invocation reports exactly one window of records (e.g. one fuzz program).
 *
 * Usage: xhcov-reader <shm_prefix> [--vcpus N] [--state <file>] [--reset]
 *
 * The shm files are mmap'd read-only; nothing is ever written back to them.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define XHCOV_MAGIC         0x313030564f434858ULL /* "XHCOV001" as LE bytes */
#define XHCOV_FMT_VERSION   1ULL
#define XHCOV_HDR_BYTES     4096ULL
#define XHCOV_MAX_VCPUS     4096u
#define XHCOV_PROBE_DEFAULT 8u

/* Must match struct xhcov_header in libxhcov.c, field for field. */
struct xhcov_header {
    uint64_t magic;
    uint64_t version;
    uint64_t cpu_index;
    uint64_t capacity;
    uint64_t write_cursor; /* monotonic; published with release store */
    uint64_t loss_count;     /* records overwritten by wrap-around */
    uint64_t reserved[8];
};

struct vcpu_report {
    int present;
    uint64_t cursor;  /* writer cursor observed this run */
    uint64_t drained; /* records printed this run */
    uint64_t loss;    /* header loss_count (writer-side overwrites) */
    uint64_t misses;  /* reader-side gap: cursor - last - capacity */
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <shm_prefix> [--vcpus N] [--state <file>] [--reset]\n"
            "\n"
            "  <shm_prefix>  plugin shm prefix; drains <prefix>.<idx> files\n"
            "  --vcpus N     probe vCPU indices 0..N-1 (default 0..7)\n"
            "  --state FILE  last-cursor state file (default <prefix>.state)\n"
            "  --reset       ignore saved state and drain from cursor 0\n"
            "                (bounded by the ring capacity)\n",
            prog);
}

static void read_state(const char *path, uint64_t *cursors, unsigned int n)
{
    FILE *f = fopen(path, "r");
    unsigned int idx;
    uint64_t cur;

    if (f == NULL) {
        return; /* first run: everything starts from cursor 0 */
    }
    while (fscanf(f, "%u %" SCNu64, &idx, &cur) == 2) {
        if (idx < n) {
            cursors[idx] = cur;
        }
    }
    fclose(f);
}

static int save_state(const char *path, const struct vcpu_report *rep,
                      unsigned int n)
{
    size_t len = strlen(path) + sizeof(".tmp");
    char *tmp = malloc(len);
    FILE *f;
    unsigned int i;
    int rc = 0;

    if (tmp == NULL) {
        fprintf(stderr, "xhcov-reader: out of memory\n");
        return -1;
    }
    snprintf(tmp, len, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (f == NULL) {
        fprintf(stderr, "xhcov-reader: fopen(%s): %s\n", tmp, strerror(errno));
        free(tmp);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (rep[i].present &&
            fprintf(f, "%u %" PRIu64 "\n", i, rep[i].cursor) < 0) {
            rc = -1;
            break;
        }
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "xhcov-reader: write %s: %s\n", tmp, strerror(errno));
        rc = -1;
    }
    if (rc == 0 && rename(tmp, path) != 0) {
        fprintf(stderr, "xhcov-reader: rename(%s, %s): %s\n", tmp, path,
                strerror(errno));
        rc = -1;
    }
    free(tmp);
    return rc;
}

/*
 * Drain one vCPU ring. Returns 1 if the file was present and valid,
 * 0 if absent/empty (skip), -1 on error.
 */
static int drain_vcpu(unsigned int idx, const char *prefix, uint64_t last,
                      struct vcpu_report *rep)
{
    char path[PATH_MAX];
    int fd;
    struct stat st;
    void *map;
    const struct xhcov_header *h;
    const volatile uint64_t *slots;
    uint64_t cap, cursor, start, i;

    if (snprintf(path, sizeof(path), "%s.%u", prefix, idx)
        >= (int)sizeof(path)) {
        fprintf(stderr, "xhcov-reader: vcpu %u: path too long\n", idx);
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return 0; /* vCPU not present */
        }
        fprintf(stderr, "xhcov-reader: open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "xhcov-reader: fstat(%s): %s\n", path,
                strerror(errno));
        close(fd);
        return -1;
    }
    if ((uint64_t)st.st_size < XHCOV_HDR_BYTES) {
        fprintf(stderr, "xhcov-reader: %s: too small (%" PRId64 " bytes)\n",
                path, (int64_t)st.st_size);
        close(fd);
        return -1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        fprintf(stderr, "xhcov-reader: mmap(%s): %s\n", path, strerror(errno));
        return -1;
    }
    h = (const struct xhcov_header *)map;

    if (__atomic_load_n(&h->magic, __ATOMIC_RELAXED) != XHCOV_MAGIC) {
        fprintf(stderr, "xhcov-reader: %s: bad magic 0x%016" PRIx64 "\n",
                path, h->magic);
        munmap(map, (size_t)st.st_size);
        return -1;
    }
    if (__atomic_load_n(&h->version, __ATOMIC_RELAXED) != XHCOV_FMT_VERSION) {
        fprintf(stderr, "xhcov-reader: %s: unsupported version %" PRIu64
                        " (expected %" PRIu64 ")\n",
                path, h->version, (uint64_t)XHCOV_FMT_VERSION);
        munmap(map, (size_t)st.st_size);
        return -1;
    }
    cap = __atomic_load_n(&h->capacity, __ATOMIC_ACQUIRE);
    if (cap == 0 ||
        cap > ((uint64_t)st.st_size - XHCOV_HDR_BYTES) / sizeof(uint64_t)) {
        fprintf(stderr, "xhcov-reader: %s: bad capacity %" PRIu64
                        " for file size %" PRId64 "\n",
                path, cap, (int64_t)st.st_size);
        munmap(map, (size_t)st.st_size);
        return -1;
    }

    /* acquire: pairs with the plugin's release store of the cursor */
    cursor = __atomic_load_n(&h->write_cursor, __ATOMIC_ACQUIRE);
    if (cursor < last) {
        /* ring was re-created (fresh QEMU run): restart from empty */
        last = 0;
    }

    start = last;
    if (cursor - last > cap) {
        /*
         * The writer lapped us: records (last, cursor - cap] are gone.
         * Report the gap and drain the oldest still-live window.
         */
        rep->misses = cursor - last - cap;
        start = cursor - cap;
        fprintf(stderr,
                "xhcov-reader: vcpu=%u ring wrapped: %" PRIu64
                " record(s) missed (drain more often)\n",
                idx, rep->misses);
    }

    slots = (const volatile uint64_t *)((const char *)map + XHCOV_HDR_BYTES);
    for (i = start; i < cursor; i++) {
        uint64_t pc = __atomic_load_n(&slots[i % cap], __ATOMIC_RELAXED);
        printf("vcpu=%u pc=0x%016" PRIx64 "\n", idx, pc);
    }

    rep->present = 1;
    rep->cursor = cursor;
    rep->drained = cursor - start;
    rep->loss = __atomic_load_n(&h->loss_count, __ATOMIC_RELAXED);

    munmap(map, (size_t)st.st_size);
    return 1;
}

int main(int argc, char **argv)
{
    const char *prefix;
    const char *state_path = NULL;
    unsigned int n_vcpus = XHCOV_PROBE_DEFAULT;
    int reset = 0;
    int i;
    int found = 0;
    int rc = 0;
    char default_state[PATH_MAX];
    uint64_t *cursors;
    struct vcpu_report *reps;
    unsigned int idx;

    if (argc < 2 || argv[1][0] == '-') {
        usage(argv[0]);
        return 2;
    }
    prefix = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--vcpus") == 0) {
            unsigned long v;
            char *end;

            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            errno = 0;
            v = strtoul(argv[i], &end, 10);
            if (errno != 0 || *end != '\0' || v == 0 || v > XHCOV_MAX_VCPUS) {
                fprintf(stderr, "xhcov-reader: bad --vcpus '%s'\n", argv[i]);
                return 2;
            }
            n_vcpus = (unsigned int)v;
        } else if (strcmp(argv[i], "--state") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            state_path = argv[i];
        } else if (strcmp(argv[i], "--reset") == 0) {
            reset = 1;
        } else {
            fprintf(stderr, "xhcov-reader: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (state_path == NULL) {
        if (snprintf(default_state, sizeof(default_state), "%s.state", prefix)
            >= (int)sizeof(default_state)) {
            fprintf(stderr, "xhcov-reader: state path too long\n");
            return 1;
        }
        state_path = default_state;
    }

    cursors = calloc(n_vcpus, sizeof(*cursors));
    reps = calloc(n_vcpus, sizeof(*reps));
    if (cursors == NULL || reps == NULL) {
        fprintf(stderr, "xhcov-reader: out of memory\n");
        free(cursors);
        free(reps);
        return 1;
    }
    if (!reset) {
        read_state(state_path, cursors, n_vcpus);
    }

    for (idx = 0; idx < n_vcpus; idx++) {
        int r = drain_vcpu(idx, prefix, cursors[idx], &reps[idx]);

        if (r > 0) {
            found++;
        } else if (r < 0) {
            rc = 1;
        }
    }
    if (found == 0) {
        fprintf(stderr,
                "xhcov-reader: no usable vCPU ring found for prefix '%s'"
                " (probed %u indices)\n", prefix, n_vcpus);
        free(cursors);
        free(reps);
        return 1;
    }

    for (idx = 0; idx < n_vcpus; idx++) {
        if (!reps[idx].present) {
            continue;
        }
        printf("summary vcpu=%u cursor=%" PRIu64 " drained=%" PRIu64
               " loss=%" PRIu64 " misses=%" PRIu64 "\n",
               idx, reps[idx].cursor, reps[idx].drained, reps[idx].loss,
               reps[idx].misses);
    }

    if (save_state(state_path, reps, n_vcpus) != 0) {
        rc = 1;
    }

    free(cursors);
    free(reps);
    return rc;
}
