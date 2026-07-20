/* util.c — implementation of the shared infrastructure.
 * See util.h for the contracts; this file explains the how and the why. */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════════════
 * Fatal errors and memory
 * ═══════════════════════════════════════════════════════════════════ */

/* Print "fatal: ..." and terminate.
 *
 * Everything goes to stderr, not stdout, so that piping a command's
 * output somewhere does not swallow the error message. */
void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);      /* the v* variant takes a va_list */
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* malloc or die. See util.h for why malloc(0) is avoided. */
void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

/* realloc or die.
 *
 * Note this deliberately does NOT do the "save the old pointer in case
 * realloc fails" dance: if realloc fails we exit, so there is nothing to
 * recover and nothing to leak. */
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%zu bytes)", n);
    return q;
}

/* strdup or die.
 *
 * Written by hand rather than calling strdup() because strdup is POSIX,
 * not ISO C: it is only visible with the right feature-test macros, and
 * relying on it would make the build fussier for no benefit. */
char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;       /* + 1 for the NUL */
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ══════════════════════════════════════════════════════════════════════
 * Growable byte buffer
 * ═══════════════════════════════════════════════════════════════════ */

void buf_init(buf_t *b) { b->data = NULL; b->len = 0; b->cap = 0; }

/* Ensure `need` more bytes fit, growing the allocation if necessary.
 *
 * THE IMPORTANT PART: capacity DOUBLES; it does not grow by exactly what
 * is needed. That single choice is the difference between quadratic and
 * linear behaviour:
 *
 *   grow-by-need : N appends -> N reallocs, each possibly copying the
 *                  whole buffer          -> O(N^2) total
 *   doubling     : N appends -> log2(N) reallocs, and the copies sum to
 *                  N + N/2 + N/4 + ... < 2N   -> O(N) total,
 *                  i.e. O(1) AMORTIZED per append
 *
 * "Amortized" means an individual append can be expensive (the one that
 * triggers the realloc), but the average over N appends is constant.
 * This is exactly how C++'s std::vector and Python's list grow. */
static void buf_grow(buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return;        /* fast path: already fits */
    size_t cap = b->cap ? b->cap : 64;          /* first allocation: 64 B  */
    while (cap < b->len + need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

/* Append raw bytes. Binary-safe: n is explicit, no NUL scanning. */
void buf_put(buf_t *b, const void *p, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_putc(buf_t *b, int ch) {
    /* Take an int (like fputc) but store one byte. The cast is what makes
     * values above 127 land as the caller intended on platforms where
     * plain char is signed. */
    unsigned char c = (unsigned char)ch;
    buf_put(b, &c, 1);
}

/* Append a string without its terminator — see the note in util.h. */
void buf_putstr(buf_t *b, const char *s) { buf_put(b, s, strlen(s)); }

/* ── big-endian writers ────────────────────────────────────────────────
 * Each byte is shifted out of the value explicitly, most significant
 * first. No memcpy of the integer, no union tricks, no htonl: the output
 * depends only on arithmetic, so it is identical on every architecture. */

void buf_put16be(buf_t *b, uint16_t v) {
    unsigned char p[2] = { (unsigned char)(v >> 8), (unsigned char)v };
    buf_put(b, p, 2);
}

void buf_put32be(buf_t *b, uint32_t v) {
    unsigned char p[4];
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i));
    buf_put(b, p, 4);
}

void buf_put64be(buf_t *b, uint64_t v) {
    unsigned char p[8];
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (56 - 8 * i));
    buf_put(b, p, 8);
}

void buf_free(buf_t *b) { free(b->data); buf_init(b); }

/* ══════════════════════════════════════════════════════════════════════
 * Big-endian readers
 * ═══════════════════════════════════════════════════════════════════ */

/* Rebuild integers from bytes, most significant first.
 *
 * The value is shifted left and OR'd byte by byte, which — like the
 * writers — never touches the host's representation of the integer, so
 * a file written on x86 reads back identically on ARM or SPARC. */

uint16_t get16be(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

uint32_t get32be(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | p[i];
    return v;
}

uint64_t get64be(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

/* ══════════════════════════════════════════════════════════════════════
 * File I/O
 * ═══════════════════════════════════════════════════════════════════ */

/* Read an entire file into a fresh buffer.
 *
 * The size is discovered by reading rather than by stat()ing first. That
 * is deliberate: a stat-then-read has a race (the file can change size in
 * between) and does not work for things whose size stat cannot report.
 * Reading until EOF is always correct.
 *
 * The buffer doubles as it fills — same amortized-O(1) reasoning as
 * buf_grow — then is shrunk to the exact size at the end. */
unsigned char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");    /* "b" matters on Windows, harmless here */
    if (!f) return NULL;

    size_t cap = 8192, len = 0;
    unsigned char *buf = xmalloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; buf = xrealloc(buf, cap); }
        size_t r = fread(buf + len, 1, cap - len, f);
        len += r;
        if (r == 0) {
            /* fread returning 0 means EOF *or* error — they must be told
             * apart, otherwise a read failure silently truncates data. */
            if (ferror(f)) { fclose(f); free(buf); return NULL; }
            break;                                  /* clean EOF */
        }
    }
    fclose(f);

    buf = xrealloc(buf, len + 1);   /* shrink to fit, + 1 for the extra NUL */
    buf[len] = 0;                   /* convenience terminator, not in *n    */
    *n = len;
    return buf;
}

/* Write a file atomically: full write to a temp file, then rename.
 *
 * Why the temp file at all? Because open(path, O_TRUNC) destroys the old
 * contents *immediately*, before the new bytes exist. Any failure after
 * that point — a crash, a full disk, a lost power supply — leaves the
 * file truncated or half-written. For .mygit/index that means a corrupt
 * repository with no way back.
 *
 * rename() is atomic within a filesystem, so `path` only ever contains
 * the complete old file or the complete new one. This pattern is used by
 * databases, browsers and editors for exactly this reason.
 *
 * The .tmp file is unlinked on every failure path so a failed write does
 * not leave litter behind. */
int write_file_atomic(const char *path, const void *data, size_t n, mode_t mode) {
    char tmp[4096];
    /* snprintf returns what it WANTED to write, so >= sizeof means the
     * name was truncated — fail rather than write to the wrong path. */
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    /* write() may transfer fewer bytes than requested (it is allowed to,
     * especially on pipes and sockets), so it must be looped. Assuming a
     * single write() covers everything is a classic silent-corruption bug. */
    const unsigned char *p = data;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)w;
    }

    /* close() can fail (deferred write errors surface here), so its return
     * value is checked too — not just the writes. */
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }

    /* chmod after rename: the mode is applied to the file now at `path`. */
    chmod(path, mode);
    return 0;
}

int path_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir -p: create every missing component of a directory path.
 *
 * The trick is to walk the copy and temporarily terminate the string at
 * each '/', creating the prefix, then restore the slash and continue.
 * EEXIST is not an error — the whole point is to be idempotent. */
int mkdir_p(const char *dirpath) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s", dirpath) >= (int)sizeof tmp) return -1;

    /* Start at tmp + 1: if the path is absolute, the leading '/' must not
     * be turned into an attempt to mkdir(""). */
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;                                     /* cut here            */
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';                                   /* put it back         */
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;   /* last one */
    return 0;
}

/* Create the parents of a file path: chop at the last '/' and mkdir_p. */
int mkdirs_for(const char *filepath) {
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s", filepath) >= (int)sizeof tmp) return -1;
    char *sl = strrchr(tmp, '/');
    if (!sl) return 0;              /* no directory part: nothing to do */
    *sl = 0;
    return mkdir_p(tmp);
}

/* ══════════════════════════════════════════════════════════════════════
 * Hex conversion
 * ═══════════════════════════════════════════════════════════════════ */

/* Raw bytes -> lowercase hex string.
 *
 * Each byte becomes two characters: the high nibble (>> 4) and the low
 * nibble (& 15), each indexed into the digit table. */
void hex_from_raw(const unsigned char *raw, size_t n, char *hex) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        hex[2 * i]     = d[raw[i] >> 4];
        hex[2 * i + 1] = d[raw[i] & 15];
    }
    hex[2 * n] = 0;
}

/* One hex character -> its value, or -1 if it is not a hex digit.
 *
 * Written as explicit range checks rather than with isxdigit() + arithmetic
 * because the "subtract '0'" trick assumes ASCII contiguity, and because
 * this way the invalid case is a value, not undefined behaviour. */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Hex string -> raw bytes. Rejects odd lengths and non-hex input.
 * Validation matters: this is fed hashes that came off disk or from the
 * command line, neither of which is trustworthy. */
int raw_from_hex(const char *hex, size_t hexlen, unsigned char *raw) {
    if (hexlen % 2) return -1;                      /* needs whole bytes */
    for (size_t i = 0; i < hexlen / 2; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        raw[i] = (unsigned char)(hi << 4 | lo);     /* recombine nibbles */
    }
    return 0;
}

int is_hex(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (hexval(s[i]) < 0) return 0;
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════
 * String vector
 * ═══════════════════════════════════════════════════════════════════ */

/* Append, taking ownership. Same doubling growth as buf_grow. */
void sv_push(strvec *s, char *owned) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = xrealloc(s->v, s->cap * sizeof *s->v);
    }
    s->v[s->n++] = owned;
}

/* qsort comparator.
 *
 * The double indirection is the awkward part: qsort hands the comparator
 * pointers to ELEMENTS, and the elements are themselves char*, so `a` is
 * really a char** that must be cast and dereferenced. Getting this wrong
 * (comparing the pointers instead of the strings) compiles cleanly and
 * sorts by memory address — a bug that only shows up as mysterious
 * ordering. */
static int cmpstr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sort in place.
 *
 * The n check is not defensive noise: an empty strvec has v == NULL, and
 * passing NULL to qsort is undefined behaviour even with count 0. It runs
 * fine on glibc — UBSan caught this exact call and it was fixed here.
 * "Works on my machine" and "is correct" are different claims. */
void sv_sort(strvec *s) {
    if (s->n) qsort(s->v, s->n, sizeof *s->v, cmpstr);
}

/* Free every owned string, then the array. Resets to a reusable empty
 * state so a double sv_free is harmless rather than a double free. */
void sv_free(strvec *s) {
    for (size_t i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL; s->n = s->cap = 0;
}
