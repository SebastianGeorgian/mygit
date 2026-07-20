/* sha256.h — SHA-256 (FIPS 180-4), implemented from scratch.
 *
 * WHY IT IS HERE
 * This is the foundation the whole repository stands on: the name of every
 * object IS the SHA-256 of its contents. Four properties make that work:
 *
 *   deterministic          same input -> same output, always.
 *                          Without this, content addressing cannot exist.
 *   avalanche              flip one input bit -> ~half the output bits
 *                          change. Any modification is unmissable.
 *   collision resistance   no two different inputs share a hash.
 *                          Without this, two different files would
 *                          silently overwrite each other on disk.
 *   preimage resistance    the input cannot be recovered from the hash.
 *                          (Not needed here, but it is what makes the
 *                          function "cryptographic" rather than a checksum.)
 *
 * WHY SHA-256 AND NOT SHA-1
 * Git used SHA-1 from 1995. In 2017 Google published SHAttered: two
 * different PDFs with the same SHA-1. For a version control system that
 * is existential — someone could substitute one file for another and keep
 * the hash. Git is migrating to SHA-256; this project starts there.
 *
 * WHY IT IS HAND-WRITTEN
 * Linking OpenSSL for one function would add a dependency to a project
 * whose point is to have none — and implementing it is a good exercise in
 * bit manipulation, streaming state and specification-following.
 *
 * WHAT YOU DO NOT NEED TO MEMORIZE
 * Where the constants come from, or why the rotations are 7/18/3. Treat
 * the compression function as a black box with a well-defined contract;
 * that is how a working engineer treats it too.
 */
#ifndef MYGIT_SHA256_H
#define MYGIT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_RAW 32       /* digest size in bytes                       */
#define SHA256_HEX 64       /* digest size as hex characters (2 per byte) */

/* Hashing state. Exposed (rather than opaque) so callers can put it on the
 * stack — there is no allocation anywhere in this module. */
typedef struct {
    uint32_t h[8];          /* the running digest, updated per block      */
    uint64_t len;           /* total bytes fed in so far (needed for the
                             * final padding — see sha256_final)          */
    uint8_t  buf[64];       /* holds a partial block between updates      */
    size_t   buflen;        /* bytes currently in buf (0..63)             */
} sha256_ctx;

/* Set the initial state. Must be called before any update. */
void sha256_init(sha256_ctx *c);

/* Feed in more data. Callable any number of times with any sizes.
 *
 * This is what makes the API streaming: the context is 100-odd bytes and
 * never grows, so a 10 GB file can be hashed with a fixed 64-byte window.
 * Blocks are processed as they complete; leftovers wait in buf. */
void sha256_update(sha256_ctx *c, const void *data, size_t n);

/* Finish: pad, append the length, and write the 32-byte digest to out.
 * The context must not be used again afterwards without re-init. */
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_RAW]);

/* Convenience one-shot: init + update + final + hex encode.
 * `out` must have room for 65 bytes (64 hex chars + NUL). */
void sha256_hex_digest(const void *data, size_t n, char out[SHA256_HEX + 1]);

#endif
