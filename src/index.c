/* index.c — reading, writing and mutating the staging area.
 * See index.h for the format and the reasoning behind it. */
#include "index.h"
#include "repo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDX_MAGIC "MGIX"
#define IDX_VERSION 1u

/* Binary search: index of the first entry whose path is >= `path`.
 *
 * This is the C++ std::lower_bound contract, and it is more useful than a
 * plain "found / not found" search: the answer doubles as the position
 * where a new entry BELONGS. index_find and index_set both use it — one
 * function, two jobs.
 *
 * NOTE `lo + (hi - lo) / 2` INSTEAD OF `(lo + hi) / 2`.
 * The naive form can overflow when both are large, producing a garbage
 * midpoint. This bug lived in java.util.Arrays.binarySearch for nine
 * years (found by Joshua Bloch in 2006) and in countless textbooks. It
 * cannot bite at this scale — but it costs nothing to write correctly,
 * and an interviewer who spots it notices something good.
 *
 * O(log n) instead of O(n): on a 50,000-file index that is ~16 string
 * comparisons rather than 50,000. */
static size_t lower_bound(const index_t *ix, const char *path) {
    size_t lo = 0, hi = ix->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(ix->e[mid].path, path) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Parse .mygit/index into memory.
 *
 * DESERIALIZATION IS WHERE YOU TREAT DISK AS HOSTILE. Every read is
 * bounds-checked before it happens, because the file may be truncated,
 * corrupted or hand-edited, and a length field read out of it must never
 * be trusted enough to index memory with. Each `off + ... > n` check is
 * what stands between a corrupt file and a buffer overrun. */
void index_load(index_t *ix) {
    ix->e = NULL;
    ix->n = ix->cap = 0;

    size_t n;
    unsigned char *buf = read_file(INDEX_FILE, &n);
    if (!buf) return;                           /* no index yet: empty */

    /* Header: magic + version. Fail loudly rather than misinterpret. */
    if (n < 12 || memcmp(buf, IDX_MAGIC, 4) != 0)
        die("corrupt index (bad magic)");
    if (get32be(buf + 4) != IDX_VERSION)
        die("unsupported index version");

    uint32_t cnt = get32be(buf + 8);
    size_t off = 12;

    for (uint32_t i = 0; i < cnt; i++) {
        /* Fixed part: 4 + 8 + 8 + 32 + 2 bytes. Check BEFORE reading. */
        if (off + 4 + 8 + 8 + 32 + 2 > n) die("corrupt index (truncated)");

        idx_entry e;
        e.mode  = get32be(buf + off); off += 4;
        e.mtime = get64be(buf + off); off += 8;
        e.size  = get64be(buf + off); off += 8;
        hex_from_raw(buf + off, 32, e.hash); off += 32;   /* raw -> hex */

        uint16_t plen = get16be(buf + off); off += 2;
        if (off + plen > n) die("corrupt index (bad path length)");

        /* The path is NOT NUL-terminated on disk (its length is known),
         * so terminate it here for use as a C string. */
        e.path = xmalloc((size_t)plen + 1);
        memcpy(e.path, buf + off, plen);
        e.path[plen] = 0;
        off += plen;

        if (ix->n == ix->cap) {                 /* same doubling growth */
            ix->cap = ix->cap ? ix->cap * 2 : 16;
            ix->e = xrealloc(ix->e, ix->cap * sizeof *ix->e);
        }
        ix->e[ix->n++] = e;
    }
    free(buf);
}

/* Serialize the index and write it atomically.
 *
 * Everything is assembled in a buffer first and handed to
 * write_file_atomic as one blob, so a crash can never leave a half-written
 * index — which would be an unrecoverable repository. */
void index_save(const index_t *ix) {
    buf_t b;
    buf_init(&b);

    buf_put(&b, IDX_MAGIC, 4);
    buf_put32be(&b, IDX_VERSION);
    buf_put32be(&b, (uint32_t)ix->n);

    for (size_t i = 0; i < ix->n; i++) {
        const idx_entry *e = &ix->e[i];
        buf_put32be(&b, e->mode);
        buf_put64be(&b, e->mtime);
        buf_put64be(&b, e->size);

        /* hex -> raw: 32 bytes on disk instead of 64. */
        unsigned char raw[32];
        if (raw_from_hex(e->hash, 64, raw) != 0)
            die("corrupt in-memory index entry for %s", e->path);
        buf_put(&b, raw, 32);

        /* Length-prefix the path. The 0xFFFF check is what makes the
         * u16 cast below safe rather than a silent truncation. */
        size_t plen = strlen(e->path);
        if (plen > 0xFFFF) die("path too long: %s", e->path);
        buf_put16be(&b, (uint16_t)plen);
        buf_put(&b, e->path, plen);
    }

    if (write_file_atomic(INDEX_FILE, b.data, b.len, 0644) != 0)
        die("cannot write index");
    buf_free(&b);
}

/* Look up a path. O(log n) thanks to the sorted invariant. */
idx_entry *index_find(const index_t *ix, const char *path) {
    size_t i = lower_bound(ix, path);
    if (i < ix->n && strcmp(ix->e[i].path, path) == 0)
        return &ix->e[i];
    return NULL;
}

/* Insert a new entry or update an existing one, keeping the array sorted.
 *
 * Two cases:
 *   already present -> overwrite the fields in place. The path string is
 *                      reused, so no allocation and no free.
 *   new             -> shift the tail right by one and drop it in.
 *
 * memmove, NOT memcpy: source and destination overlap (we are sliding
 * elements inside one array). memcpy on overlapping regions is undefined
 * behaviour — it may appear to work, then corrupt data when the compiler
 * vectorizes it differently. memmove is defined to handle the overlap.
 *
 * Cost: O(n) per insert because of the shift. Fine for thousands of
 * files; for millions you would want a tree. Knowing that trade-off is
 * more valuable than pre-emptively optimizing it. */
void index_set(index_t *ix, const char *path, const char *hash,
               uint32_t mode, uint64_t mtime, uint64_t size) {
    size_t i = lower_bound(ix, path);

    if (i < ix->n && strcmp(ix->e[i].path, path) == 0) {
        idx_entry *e = &ix->e[i];               /* update in place */
        memcpy(e->hash, hash, 65);
        e->mode = mode; e->mtime = mtime; e->size = size;
        return;
    }

    if (ix->n == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 16;
        ix->e = xrealloc(ix->e, ix->cap * sizeof *ix->e);
    }
    /* Open a hole at i by sliding [i, n) one slot right. */
    memmove(ix->e + i + 1, ix->e + i, (ix->n - i) * sizeof *ix->e);

    idx_entry *e = &ix->e[i];
    e->path = xstrdup(path);                    /* the index owns its paths */
    memcpy(e->hash, hash, 65);
    e->mode = mode; e->mtime = mtime; e->size = size;
    ix->n++;
}

/* Remove an entry: free its path, then close the hole. */
int index_remove(index_t *ix, const char *path) {
    size_t i = lower_bound(ix, path);
    if (i >= ix->n || strcmp(ix->e[i].path, path) != 0) return 0;

    free(ix->e[i].path);                        /* the only owner */
    memmove(ix->e + i, ix->e + i + 1, (ix->n - i - 1) * sizeof *ix->e);
    ix->n--;
    return 1;
}

void index_free(index_t *ix) {
    for (size_t i = 0; i < ix->n; i++) free(ix->e[i].path);
    free(ix->e);
    ix->e = NULL; ix->n = ix->cap = 0;
}
