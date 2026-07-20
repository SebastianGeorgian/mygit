/* commit.c — creating and parsing commit objects. */
#include "commit.h"
#include "object.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Who is making this commit.
 *
 * MYGIT_AUTHOR first so the value can be set explicitly (scripts, tests,
 * reproducible builds), then USER, then a defined fallback. Never NULL:
 * the callers should not have to think about it.
 *
 * The returned pointer is into the environment and must not be freed. */
static const char *author_name(void) {
    const char *a = getenv("MYGIT_AUTHOR");
    if (a && *a) return a;                      /* set AND non-empty */
    a = getenv("USER");
    if (a && *a) return a;
    return "unknown";
}

/* Assemble the commit text and store it.
 *
 * The header order (tree, parent, author) is fixed. That matters more
 * than it looks: the object id is the hash of these exact bytes, so
 * emitting the same information in a different order would produce a
 * different commit id for an identical commit. Serialization formats need
 * a canonical form. */
void commit_create(const char *tree_hex, const char *parent_hex,
                   const char *message, char out_hex[65]) {
    buf_t b;
    buf_init(&b);

    char line[512];
    snprintf(line, sizeof line, "tree %s\n", tree_hex);
    buf_putstr(&b, line);

    /* No parent line at all on the first commit — an empty "parent \n"
     * would be a different thing to parse, and the absence is what marks
     * the root of the history. */
    if (parent_hex && *parent_hex) {
        snprintf(line, sizeof line, "parent %s\n", parent_hex);
        buf_putstr(&b, line);
    }

    /* %lld with the cast because time_t's actual type is not fixed by the
     * standard (long on many systems, 64-bit elsewhere); casting makes
     * the format specifier correct everywhere. */
    snprintf(line, sizeof line, "author %s %lld\n",
             author_name(), (long long)time(NULL));
    buf_putstr(&b, line);

    buf_putc(&b, '\n');                         /* blank line = end of headers */
    buf_putstr(&b, message);

    /* Normalize: the message always ends with exactly one newline, so
     * `commit -m "x"` and `commit -m "x\n"` produce the same object
     * instead of two commits that differ by one invisible byte. */
    if (b.len == 0 || b.data[b.len - 1] != '\n') buf_putc(&b, '\n');

    obj_write("commit", b.data, b.len, out_hex);
    buf_free(&b);
}

/* Parse a commit object into commit_info.
 *
 * The payload from obj_read has a NUL past the end, so it can be walked
 * with ordinary string functions — and this parser edits the buffer in
 * place, writing NULs over the newlines to terminate each header line.
 * That is why `raw` is copied from where needed before being freed at the
 * end, and why ci->message is strdup'd rather than pointing into it.
 *
 * Unknown headers are skipped rather than rejected: a newer version could
 * add a field, and refusing to read a commit because of it would be worse
 * than ignoring what we do not understand. */
void commit_parse(const char *hex, commit_info *ci) {
    char type[16];
    size_t n;
    unsigned char *raw = obj_read(hex, type, &n);
    if (!raw) die("object %s not found", hex);
    if (strcmp(type, "commit") != 0) die("object %s is not a commit", hex);

    memset(ci, 0, sizeof *ci);                  /* parent[0] = 0 == "no parent" */
    char *p = (char *)raw;

    /* Header lines, until the blank line that separates the message. */
    while (*p && *p != '\n') {
        char *eol = strchr(p, '\n');
        if (!eol) break;                        /* no newline: truncated */
        *eol = 0;                               /* terminate this line */

        /* The strlen == 64 checks reject malformed hashes here, so that
         * nothing downstream has to wonder whether ci->tree is a hash. */
        if (strncmp(p, "tree ", 5) == 0 && strlen(p + 5) == 64)
            memcpy(ci->tree, p + 5, 65);
        else if (strncmp(p, "parent ", 7) == 0 && strlen(p + 7) == 64)
            memcpy(ci->parent, p + 7, 65);
        else if (strncmp(p, "author ", 7) == 0) {
            /* "author <name> <timestamp>" — the name may contain spaces,
             * so split at the LAST space (strrchr), not the first. */
            char *last = strrchr(p + 7, ' ');
            if (last) {
                size_t nl = (size_t)(last - (p + 7));
                if (nl >= sizeof ci->author) nl = sizeof ci->author - 1;
                memcpy(ci->author, p + 7, nl);
                ci->author[nl] = 0;
                ci->when = (time_t)strtoll(last + 1, NULL, 10);
            }
        }
        p = eol + 1;                            /* next line */
    }

    if (*p == '\n') p++;                        /* skip the blank line */
    ci->message = xstrdup(p);                   /* own copy: raw is freed */

    /* A commit without a tree is not a commit. Fail here, where the cause
     * is obvious, rather than three calls later on an empty hash. */
    if (!ci->tree[0]) die("commit %s has no tree", hex);
    free(raw);
}
