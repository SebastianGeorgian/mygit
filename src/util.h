/* util.h — shared infrastructure used by every other module.
 *
 * Nothing here knows anything about Git: these are the generic tools the
 * rest of the code is built out of — fatal errors, a growable byte buffer,
 * binary-safe file I/O, hex encoding, big-endian serialization primitives
 * and a small vector of owned strings.
 *
 * Read this header first. It is the vocabulary every other file speaks.
 */
#ifndef MYGIT_UTIL_H
#define MYGIT_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ── fatal errors and memory ───────────────────────────────────────────
 *
 * Allocation failure is not something this program can meaningfully
 * recover from, so instead of returning NULL up through forty call sites
 * (and getting the check wrong in one of them), the x* wrappers abort.
 * Every other module can then assume allocation succeeded, which keeps
 * the calling code readable.
 */

/* Print "fatal: <message>" to stderr and exit(1). Never returns.
 * The format attribute makes the compiler type-check the varargs the same
 * way it checks printf, so a %s fed an int is a compile-time warning. */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* malloc that never returns NULL (dies instead).
 * Note it requests at least 1 byte: malloc(0) is implementation-defined
 * (it may return NULL *or* a valid pointer, both are legal), and that
 * ambiguity is not worth carrying around. */
void *xmalloc(size_t n);

/* realloc that never returns NULL (dies instead). */
void *xrealloc(void *p, size_t n);

/* strdup that never returns NULL (dies instead). Caller owns the result. */
char *xstrdup(const char *s);

/* ── growable byte buffer ──────────────────────────────────────────────
 *
 * A length-tracked byte array that grows on demand. Used to assemble
 * object payloads and the index before writing them out.
 *
 * It stores LENGTH, not a NUL terminator, so it is safe for binary data:
 * a zero byte in the middle is just data, not the end of anything.
 */
typedef struct {
    unsigned char *data;   /* owned; NULL while empty                    */
    size_t len;            /* bytes actually used                        */
    size_t cap;            /* bytes allocated (always >= len)            */
} buf_t;

/* Initialize an empty buffer. Must be called before any buf_put*. */
void buf_init(buf_t *b);

/* Append n raw bytes. Binary-safe. */
void buf_put(buf_t *b, const void *p, size_t n);

/* Append a single byte. */
void buf_putc(buf_t *b, int ch);

/* Append a C string, WITHOUT its NUL terminator.
 * (If a format needs the NUL as a separator, the caller adds it
 * explicitly with buf_putc(b, 0) — this is deliberate, so that the
 * on-disk layout is always visible in the calling code.) */
void buf_putstr(buf_t *b, const char *s);

/* Append integers in BIG-ENDIAN byte order.
 *
 * Why big-endian, and why byte by byte instead of memcpy'ing the integer?
 * Because the result must be identical on every machine. Writing the raw
 * bytes of a uint32_t would produce "01 00 00 00" on x86 (little-endian)
 * and "00 00 00 01" on a big-endian CPU — the same file would read back
 * as a different number. Shifting each byte out by hand removes the
 * machine's byte order from the equation entirely.
 *
 * Big-endian specifically because it is the network byte order convention
 * (RFC 1700) and because it reads left-to-right in a hex dump. */
void buf_put16be(buf_t *b, uint16_t v);
void buf_put32be(buf_t *b, uint32_t v);
void buf_put64be(buf_t *b, uint64_t v);

/* Release the buffer's memory and reset it to the empty state. */
void buf_free(buf_t *b);

/* ── big-endian readers ────────────────────────────────────────────────
 * The inverse of buf_put*be: read an integer back out of a byte stream,
 * independently of the host's byte order. The caller is responsible for
 * having checked that at least 2/4/8 bytes are available at p. */
uint16_t get16be(const unsigned char *p);
uint32_t get32be(const unsigned char *p);
uint64_t get64be(const unsigned char *p);

/* ── file I/O ──────────────────────────────────────────────────────── */

/* Read a whole file into memory. Binary-safe.
 *
 * Returns a malloc'd buffer (caller frees) with *n set to the file size,
 * or NULL if the file cannot be read. The buffer has one extra NUL byte
 * past *n, NOT counted in *n: that costs one byte and lets text formats
 * (commits, HEAD, refs) be parsed with ordinary string functions without
 * a separate copy, while binary callers can just ignore it. */
unsigned char *read_file(const char *path, size_t *n);

/* Write a file ATOMICALLY, then chmod it to `mode`. Returns 0 on success.
 *
 * The data is written to "<path>.tmp" and then rename()d into place.
 * rename() is atomic on POSIX within one filesystem: any observer sees
 * either the complete old file or the complete new one, never a partial
 * write. That matters because a crash (or a pulled plug) halfway through
 * writing .mygit/index would otherwise leave a corrupt repository behind;
 * with this, the worst case is a leftover .tmp file and intact data. */
int write_file_atomic(const char *path, const void *data, size_t n, mode_t mode);

/* True if the path exists (of any type). */
int path_exists(const char *p);

/* True if the path exists AND is a directory. */
int is_dir(const char *p);

/* Create a directory and all missing parents ("mkdir -p"). 0 on success.
 * Already existing directories are not an error. */
int mkdir_p(const char *dirpath);

/* Create the parent directories of a FILE path, not the file itself.
 * "a/b/c.txt" creates "a/b". A path with no '/' is a no-op success. */
int mkdirs_for(const char *filepath);

/* ── hex <-> raw bytes ─────────────────────────────────────────────────
 *
 * Hashes live in two representations, on purpose:
 *   - hex (64 chars) in memory: printable, comparable with strcmp,
 *     usable as a path component;
 *   - raw (32 bytes) on disk: half the size.
 * These convert at the boundary. */

/* Write 2*n lowercase hex chars plus a NUL into `hex`.
 * The caller must provide a buffer of at least 2*n + 1 bytes. */
void hex_from_raw(const unsigned char *raw, size_t n, char *hex);

/* Parse hexlen hex chars into hexlen/2 raw bytes. Returns 0 on success,
 * -1 if hexlen is odd or a non-hex character is present. */
int raw_from_hex(const char *hex, size_t hexlen, unsigned char *raw);

/* True if the first n characters of s are all hex digits.
 * Used to validate untrusted input (HEAD contents, user-typed hashes)
 * before it is treated as a hash. */
int is_hex(const char *s, size_t n);

/* ── vector of owned strings ───────────────────────────────────────────
 *
 * A dynamic array of char* that OWNS its strings: sv_push takes over the
 * pointer you hand it, and sv_free frees them all. Used to collect the
 * file lists that `status` prints.
 *
 * The `owned` parameter name is the ownership contract, spelled out in
 * the signature: after sv_push(&v, p), p belongs to the vector and must
 * not be freed by the caller. */
typedef struct {
    char **v;              /* owned array of owned strings */
    size_t n, cap;
} strvec;

/* Append a string. The vector takes ownership of `owned`.
 * A zeroed strvec ({0}) is a valid empty vector — no init call needed. */
void sv_push(strvec *s, char *owned);

/* Sort the strings in place with strcmp order. */
void sv_sort(strvec *s);

/* Free every string and the array itself; reset to empty. */
void sv_free(strvec *s);

#endif
