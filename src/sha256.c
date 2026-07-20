/* sha256.c — SHA-256 per FIPS 180-4.
 *
 * STRUCTURE (Merkle-Damgard construction, shared by all classic hashes):
 *
 *   message -> [pad to a multiple of 64] -> 64-byte blocks
 *                                              |
 *                    state (32 bytes) <-> [compression function]
 *                                              |
 *                                        final state = digest
 *
 * The state starts at fixed constants, each block mixes into it, and the
 * state after the last block is the answer. sha256_init / _update / _final
 * map exactly onto those three phases.
 */
#include "sha256.h"
#include "util.h"

#include <string.h>

/* Round constants: the first 32 bits of the fractional parts of the cube
 * roots of the first 64 primes (FIPS 180-4, section 4.2.2).
 *
 * These are "nothing-up-my-sleeve numbers": derived from an obvious
 * mathematical source precisely so that the designers cannot be accused
 * of having chosen values that hide a backdoor. You never need to know
 * them; you only need to know why they look the way they do. */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* Rotate right: bits shifted off the bottom reappear at the top.
 *
 * C has no rotate operator, so it is built from two shifts and an OR.
 * Rotation (unlike a plain shift) loses no bits — that is why hashes use
 * it: every input bit keeps influencing the result. */
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* The compression function: mix one 64-byte block into the state.
 *
 * Two phases:
 *   1. message schedule — expand the block's 16 words into 64 words,
 *      so that every input word affects many rounds;
 *   2. 64 rounds — stir the eight state words with the schedule and the
 *      round constants.
 * Finally the result is ADDED to the incoming state (not assigned): that
 * feedback is what makes the construction hard to invert. */
static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];

    /* 1a. Load the 16 big-endian words of this block. */
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];

    /* 1b. Extend to 64 words. Each new word blends four earlier ones,
     *     which is how a change in any input word propagates forward. */
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    /* 2. Working copy of the state. Named a..h as in the specification —
     *    matching the spec's names makes the code checkable against it.
     *    (`cc` only because `c` is the context parameter.) */
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1  = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);            /* "choose"          */
        uint32_t t1  = h + S1 + ch + K[i] + w[i];
        uint32_t S0  = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc); /* "majority"        */
        uint32_t t2  = S0 + maj;

        /* Shift everything down one slot and inject t1/t2 at the top.
         * Overflow of uint32_t here is intentional and well-defined:
         * unsigned arithmetic wraps, and the spec says "mod 2^32". */
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    /* 3. Feed the mixed values back into the running state. */
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

/* Initial state: the first 32 bits of the fractional parts of the square
 * roots of the first 8 primes — nothing-up-my-sleeve numbers again. */
void sha256_init(sha256_ctx *c) {
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->buflen = 0;
}

/* Absorb data, processing every complete 64-byte block.
 *
 * The loop handles the general case where the caller's data neither
 * starts nor ends on a block boundary: take as much as fits in buf, and
 * whenever buf fills, compress it and start over. Leftover bytes stay in
 * buf until the next update (or the final padding). */
void sha256_update(sha256_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;                    /* running total, needed by _final */
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

/* Finalize: pad the message, append its bit length, emit the digest.
 *
 * The padding rule from the spec: append a 1 bit (byte 0x80), then zeros
 * until 56 bytes into the block, then the original length as a 64-bit
 * big-endian count of BITS. Encoding the length is what stops "abc" and
 * "abc\x80\0\0..." from hashing to the same value.
 *
 * THE TRAP: `bits` is computed BEFORE any padding is fed in. The padding
 * goes through sha256_update, which increments c->len — so reading c->len
 * afterwards yields the padded length, not the message length, and every
 * digest comes out wrong. This is the single most common bug when
 * implementing SHA-256 from the specification. */
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_RAW]) {
    uint64_t bits = c->len * 8;      /* capture BEFORE padding */
    uint8_t pad = 0x80, zero = 0;

    sha256_update(c, &pad, 1);       /* the mandatory 1 bit */

    /* Zero-fill up to offset 56. If the block is already past 56, this
     * naturally spills into the next block, which is exactly what the
     * spec requires — no special case needed. */
    while (c->buflen != 56)
        sha256_update(c, &zero, 1);

    /* The length, big-endian, filling bytes 56..63 of the final block. */
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);       /* this triggers the last compression */

    /* Serialize the eight state words, big-endian, into the digest. */
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->h[i]);
    }
}

/* One-shot convenience wrapper: the form the rest of the project uses. */
void sha256_hex_digest(const void *data, size_t n, char out[SHA256_HEX + 1]) {
    sha256_ctx c;
    uint8_t raw[SHA256_RAW];
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, raw);
    hex_from_raw(raw, SHA256_RAW, out);
}
