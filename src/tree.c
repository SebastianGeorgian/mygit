/* tree.c — building tree objects from the index, and reading them back. */
#include "tree.h"
#include "object.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════
 * BUILDING: flat index -> hierarchy of tree objects
 *
 * THE PROBLEM
 *   The index is a flat, sorted list:
 *       a.txt
 *       lib/b.txt
 *       lib/sub/c.txt
 *       src/main.c
 *   A commit needs a HIERARCHY: one tree object per directory.
 *
 * THE OBSERVATION THAT SOLVES IT
 *   Because the list is sorted with strcmp, every path starting with
 *   "lib/" is CONTIGUOUS.
 *
 *   Proof: take X and Z both starting with "lib/", and any Y with
 *   X < Y < Z. Since X and Z share the prefix "lib/" and Y sits between
 *   them lexicographically, Y's first four characters are forced to be
 *   "lib/" as well. So no foreign path can appear between two paths of
 *   the same directory. []
 *
 * THE CONSEQUENCE
 *   One linear scan per level is enough to group the children. No hash
 *   map, no extra sorting, no auxiliary structures. Each directory group
 *   is recursed into first, so subtrees are written before their parents
 *   — bottom-up construction — and the parent embeds the child's hash.
 *
 * THE TRAP THIS MUST HANDLE
 *   ASCII: '.' = 46, '/' = 47, '0' = 48. So these sort as:
 *       dir.txt      <  dir/x        <  dir0
 *   "dir.txt" starts with "dir" but is NOT inside the directory "dir".
 *   Checking the prefix alone would wrongly swallow it — which is why the
 *   grouping test below also requires the '/' right after the name.
 *   (There is a test for exactly this case in test.sh.)
 * ═══════════════════════════════════════════════════════════════════ */

/* Append one "<mode> <name>\0<32-byte hash>" record to a tree payload. */
static void append_entry(buf_t *b, uint32_t mode, const char *name,
                         size_t namelen, const char *hex) {
    char modestr[16];
    snprintf(modestr, sizeof modestr, "%o", mode);      /* %o = octal */
    buf_putstr(b, modestr);
    buf_putc(b, ' ');
    /* namelen is explicit: for a directory the name is a slice of a
     * longer path ("lib" out of "lib/b.txt"), so it is not NUL-terminated
     * and strlen would run past it. */
    buf_put(b, name, namelen);
    buf_putc(b, 0);

    unsigned char raw[32];
    if (raw_from_hex(hex, 64, raw) != 0)
        die("bad hash while building tree");
    buf_put(b, raw, 32);                                /* raw, not hex */
}

/* Build one tree object from index entries [lo, hi), writing it to the
 * store and returning its id.
 *
 * `plen` is how many characters of each path belong to enclosing
 * directories and must be skipped. At the root plen == 0; inside "lib/"
 * it is 4. That is the trick that lets one function serve every level:
 * "lib/sub/c.txt" with plen == 4 simply *is* "sub/c.txt".
 *
 * Complexity: each entry is visited once per level of its own depth, so
 * O(total path components) overall. */
static void build_range(const index_t *ix, size_t lo, size_t hi,
                        size_t plen, char out_hex[65]) {
    buf_t b;
    buf_init(&b);

    size_t i = lo;
    while (i < hi) {
        const char *rest = ix->e[i].path + plen;    /* path at this level */
        const char *slash = strchr(rest, '/');

        if (!slash) {
            /* No slash left: a file directly in this directory. */
            append_entry(&b, ix->e[i].mode, rest, strlen(rest),
                         ix->e[i].hash);
            i++;
        } else {
            /* A subdirectory. Its name is rest[0 .. nlen). */
            size_t nlen = (size_t)(slash - rest);

            /* Find where this directory's group ends. Thanks to the
             * contiguity property, a single forward scan is enough — the
             * first entry that does not match ends the group, and nothing
             * belonging to it can appear later.
             *
             * The r2[nlen] == '/' test is the "dir.txt vs dir/x" guard
             * described above: the prefix matching is not sufficient on
             * its own, the separator must follow. */
            size_t j = i + 1;
            while (j < hi) {
                const char *r2 = ix->e[j].path + plen;
                if (strncmp(r2, rest, nlen) == 0 && r2[nlen] == '/') j++;
                else break;
            }

            /* Recurse into [i, j) with the prefix extended past
             * "<name>/" — this writes the child tree FIRST... */
            char sub[65];
            build_range(ix, i, j, plen + nlen + 1, sub);
            /* ...and its hash becomes data inside this tree. */
            append_entry(&b, 040000, rest, nlen, sub);
            i = j;
        }
    }

    obj_write("tree", b.data, b.len, out_hex);
    buf_free(&b);
}

void tree_build_from_index(const index_t *ix, char root_hex[65]) {
    build_range(ix, 0, ix->n, 0, root_hex);
}

/* ══════════════════════════════════════════════════════════════════════
 * FLATTENING: hierarchy of trees -> flat list of files
 * The inverse operation, used by status (compare against HEAD) and
 * checkout (materialize a snapshot).
 * ═══════════════════════════════════════════════════════════════════ */

static void flat_push(tree_flat *t, const char *path, const char *hex,
                      uint32_t mode) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->f = xrealloc(t->f, t->cap * sizeof *t->f);
    }
    tree_file *f = &t->f[t->n++];
    f->path = xstrdup(path);
    memcpy(f->hash, hex, 65);
    f->mode = mode;
}

/* Parse one tree object and recurse into its subdirectories.
 *
 * BINARY PARSING BY OFFSET. There is no strtok here and there cannot be:
 * the records have no fixed size (names vary) and the hashes are raw
 * bytes that may contain anything, including spaces and NULs. So the
 * payload is walked with an explicit offset, and every step checks it is
 * still inside the buffer before reading — corrupt input must produce an
 * error, never a read past the end.
 *
 * Record: "<mode> <name>\0" + 32 raw bytes. Parsed in exactly that order. */
static void flatten_rec(const char *hex, const char *prefix, tree_flat *out) {
    char type[16];
    size_t n;
    unsigned char *p = obj_read(hex, type, &n);
    if (!p) die("object %s not found", hex);
    if (strcmp(type, "tree") != 0) die("object %s is not a tree", hex);

    size_t off = 0;
    while (off < n) {
        /* 1. "<mode> " — scan to the space. */
        size_t s = off;
        while (off < n && p[off] != ' ') off++;
        if (off >= n) die("corrupt tree %s", hex);
        char modestr[16];
        size_t ml = off - s;
        if (ml >= sizeof modestr) die("corrupt tree %s (mode)", hex);
        memcpy(modestr, p + s, ml);
        modestr[ml] = 0;
        off++;                                  /* skip the space */

        /* 2. "<name>\0" — scan to the NUL. */
        s = off;
        while (off < n && p[off] != 0) off++;
        if (off >= n) die("corrupt tree %s (name)", hex);
        char name[1024];
        size_t nl = off - s;
        if (nl >= sizeof name) die("tree entry name too long");
        memcpy(name, p + s, nl);
        name[nl] = 0;
        off++;                                  /* skip the NUL */

        /* 3. 32 raw hash bytes -> hex. */
        if (off + 32 > n) die("corrupt tree %s (hash)", hex);
        char child[65];
        hex_from_raw(p + off, 32, child);
        off += 32;

        uint32_t mode = (uint32_t)strtoul(modestr, NULL, 8);   /* base 8 */

        /* Rebuild the full path: the prefix is what the recursion has
         * accumulated so far. The ternary avoids a leading '/' at the
         * root, where prefix is "". */
        char full[4096];
        if (snprintf(full, sizeof full, "%s%s%s",
                     prefix, *prefix ? "/" : "", name) >= (int)sizeof full)
            die("path too long in tree");

        /* A directory recurses; a file is collected. */
        if (mode == 040000) flatten_rec(child, full, out);
        else flat_push(out, full, child, mode);
    }
    free(p);
}

void tree_flatten(const char *tree_hex, tree_flat *out) {
    out->f = NULL;
    out->n = out->cap = 0;
    flatten_rec(tree_hex, "", out);             /* "" = start at the root */
}

/* Linear search.
 *
 * O(n), unlike index_find's binary search — deliberate. The list IS
 * sorted, so a binary search would work; but this is called on lists of
 * the size of one commit's file set, and keeping it obvious is worth more
 * than the constant factor. If a repository ever got big enough for this
 * to matter, the fix is three lines and the sorted invariant is already
 * there. Knowing that this is the trade-off is the point. */
tree_file *tree_flat_find(const tree_flat *t, const char *path) {
    for (size_t i = 0; i < t->n; i++)
        if (strcmp(t->f[i].path, path) == 0) return &t->f[i];
    return NULL;
}

void tree_flat_free(tree_flat *t) {
    for (size_t i = 0; i < t->n; i++) free(t->f[i].path);
    free(t->f);
    t->f = NULL;
    t->n = t->cap = 0;
}
