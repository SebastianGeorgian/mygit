/* object.c — the object store: hash it, write it once, read it back.
 * See object.h for the model; this file is the mechanics. */
#include "object.h"
#include "repo.h"
#include "sha256.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Map an object id to its path: ".mygit/objects/fd/e4f42a...".
 *
 * "%.2s" prints exactly the first 2 chars of hex (the directory) and
 * "hex + 2" is the rest (the filename). One expression, no temporary. */
static void obj_path(const char *hex, char out[4352]) {
    snprintf(out, 4352, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

/* Build "<type> <size>\0<payload>" and hash the whole thing.
 *
 * `out` is the interesting parameter: pass NULL when you only want the id
 * (obj_hash), or a buf_t when you also want the bytes (obj_write). One
 * function, two uses, and the serialized form is built exactly once —
 * hashing something different from what gets written would be a
 * catastrophic and very hard to find bug.
 *
 * On the *out = b line, ownership of the buffer TRANSFERS to the caller;
 * otherwise this function frees it. */
static void serialize(const char *type, const void *data, size_t n,
                      char hex[65], buf_t *out) {
    buf_t b;
    buf_init(&b);

    char hdr[48];                               /* "commit 18446744073709551615" fits */
    int hl = snprintf(hdr, sizeof hdr, "%s %zu", type, n);
    buf_put(&b, hdr, (size_t)hl);
    buf_putc(&b, 0);                            /* the NUL separator */
    buf_put(&b, data, n);

    sha256_hex_digest(b.data, b.len, hex);      /* hash header + payload */

    if (out) *out = b;                          /* transfer ownership */
    else buf_free(&b);
}

void obj_hash(const char *type, const void *data, size_t n, char hex[65]) {
    serialize(type, data, n, hex, NULL);
}

/* Write an object to the store.
 *
 * TWO THINGS WORTH NOTICING:
 *
 * 1. The path_exists check. If an object with this id is already on disk,
 *    its content is by definition identical (that is what content
 *    addressing means), so rewriting it would be pure waste. This is
 *    deduplication — the entire feature, in one if.
 *
 * 2. Mode 0444: read-only for everyone. Objects are immutable by design;
 *    the permission bits make that explicit to the operating system and
 *    to anyone poking around in .mygit with a text editor. */
void obj_write(const char *type, const void *data, size_t n, char hex[65]) {
    buf_t b;
    serialize(type, data, n, hex, &b);          /* &b: we want the bytes */

    char path[4352];
    obj_path(hex, path);

    if (!path_exists(path)) {                   /* content-addressed: write once */
        if (mkdirs_for(path) != 0)              /* create objects/xx/ if new */
            die("cannot create object directory for %s", hex);
        if (write_file_atomic(path, b.data, b.len, 0444) != 0)
            die("cannot write object %s", hex);
    }
    buf_free(&b);
}

/* Read an object: parse the header, validate it, return the payload.
 *
 * PARSING STEPS
 *   1. read the whole file
 *   2. find the NUL that ends the header  (memchr, not strchr — the
 *      payload is binary and may contain NULs of its own; strchr would
 *      also work here only because we search before the payload, but
 *      memchr with an explicit length is the honest tool)
 *   3. sscanf "<type> <size>" out of it
 *   4. CHECK that the declared size matches reality
 *   5. copy out the payload
 *
 * Step 4 is the free integrity check the header buys us: a truncated or
 * padded object is rejected instead of silently returning wrong data.
 *
 * The "%15s" in the sscanf is not decoration: type_out is 16 bytes, and
 * an unbounded %s on a corrupt object would overflow it. Every scanf of
 * a string into a fixed buffer needs that width. */
unsigned char *obj_read(const char *hex, char type_out[16], size_t *size) {
    char path[4352];
    obj_path(hex, path);

    size_t n;
    unsigned char *all = read_file(path, &n);
    if (!all) return NULL;                      /* missing: a normal outcome */

    unsigned char *nul = memchr(all, 0, n);
    if (!nul) { free(all); return NULL; }       /* no header terminator */

    char hdr[48];
    size_t hl = (size_t)(nul - all);            /* header length */
    if (hl >= sizeof hdr) { free(all); return NULL; }
    memcpy(hdr, all, hl);
    hdr[hl] = 0;

    size_t declared = 0;
    if (sscanf(hdr, "%15s %zu", type_out, &declared) != 2) {
        free(all);                              /* malformed header */
        return NULL;
    }

    size_t plen = n - hl - 1;                   /* - 1 for the NUL */
    if (plen != declared) { free(all); return NULL; }   /* integrity check */

    /* Copy the payload into its own allocation so the caller gets a clean
     * buffer starting at offset 0, and free the raw file. */
    unsigned char *payload = xmalloc(plen + 1);
    memcpy(payload, nul + 1, plen);
    payload[plen] = 0;                          /* convenience NUL */
    free(all);

    *size = plen;
    return payload;
}

int obj_exists(const char *hex) {
    char path[4352];
    obj_path(hex, path);
    return path_exists(path);
}

/* Expand an abbreviated hash into a full one.
 *
 * WHY THIS IS EASY: the 2-character directory prefix means we already
 * know which of the 256 directories to look in, so only its entries have
 * to be scanned — not the whole store.
 *
 * THE AMBIGUITY CHECK: `found++` inside the if means the SECOND match
 * returns -2 immediately. Reporting "not found" for an ambiguous prefix
 * would send the user hunting for a typo when the real fix is to type
 * more characters.
 *
 * The 4-character minimum matches Git and is what makes accidental
 * ambiguity unlikely; is_hex rejects input that is not a hash at all
 * before it is used to build a path. */
int obj_resolve_prefix(const char *prefix, char hex[65]) {
    size_t len = strlen(prefix);
    if (len < 4 || len > 64 || !is_hex(prefix, len)) return -1;

    if (len == 64) {                            /* already complete */
        if (!obj_exists(prefix)) return -1;
        memcpy(hex, prefix, 65);
        return 0;
    }

    char dirpath[4352];
    snprintf(dirpath, sizeof dirpath, "%s/%.2s", OBJECTS_DIR, prefix);

    DIR *d = opendir(dirpath);
    if (!d) return -1;                          /* no such prefix directory */

    const char *rest = prefix + 2;              /* the part inside the dir */
    size_t rlen = len - 2;
    char match[256] = "";                       /* d_name can be up to 255 */
    int found = 0;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;     /* skip ".", "..", leftovers */
        if (strncmp(de->d_name, rest, rlen) == 0) {
            if (found++) { closedir(d); return -2; }    /* second match */
            snprintf(match, sizeof match, "%s", de->d_name);
        }
    }
    closedir(d);

    if (!found) return -1;
    /* "%.62s" caps the filename part so the 65-byte hex buffer cannot be
     * overflowed by a bogus filename someone dropped into objects/. */
    snprintf(hex, 65, "%.2s%.62s", prefix, match);
    return 0;
}
