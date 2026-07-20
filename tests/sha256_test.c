/* sha256_test.c — validate the SHA-256 implementation against the
 * official NIST test vectors (FIPS 180-4 / NIST CAVP).
 *
 * This matters more than it looks: every object id in the repository is a
 * SHA-256 digest. If this file fails, nothing else about the project is
 * trustworthy — a subtly wrong hash still produces plausible-looking
 * output, it just quietly makes every id wrong.
 *
 * Build and run:   make check-sha
 */
#include "../src/sha256.h"

#include <stdio.h>
#include <string.h>

static int failures;

/* Compare a one-shot digest against an expected hex string. */
static void expect(const char *label, const void *data, size_t n,
                   const char *want) {
    char got[SHA256_HEX + 1];
    sha256_hex_digest(data, n, got);
    if (strcmp(got, want) == 0) {
        printf("  ok    %s\n", label);
    } else {
        printf("  FAIL  %s\n        expected %s\n        got      %s\n",
               label, want, got);
        failures++;
    }
}

int main(void) {
    puts("SHA-256 NIST test vectors:");

    /* 1. The empty message. Exercises the padding path when there is no
     *    data at all — a classic off-by-one hiding place. */
    expect("empty string", "", 0,
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    /* 2. "abc" — the canonical single-block vector from FIPS 180-4. */
    expect("\"abc\"", "abc", 3,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    /* 3. 56 bytes: exactly the length at which the length field no longer
     *    fits in the same block, forcing the padding to spill into a
     *    second one. This is the boundary case implementations get wrong. */
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        expect("56-byte message (padding spills a block)", m, strlen(m),
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }

    /* 4. One million 'a', fed in 1000-byte chunks.
     *
     *    The only vector here that tests STREAMING: it crosses thousands
     *    of block boundaries with updates that do not align to 64 bytes,
     *    which is what would expose a broken partial-block buffer or a
     *    length counter that overflows or resets. */
    {
        sha256_ctx c;
        unsigned char raw[SHA256_RAW];
        char buf[1000], got[SHA256_HEX + 1];
        memset(buf, 'a', sizeof buf);

        sha256_init(&c);
        for (int i = 0; i < 1000; i++)
            sha256_update(&c, buf, sizeof buf);
        sha256_final(&c, raw);

        static const char d[] = "0123456789abcdef";
        for (int i = 0; i < SHA256_RAW; i++) {
            got[2 * i]     = d[raw[i] >> 4];
            got[2 * i + 1] = d[raw[i] & 15];
        }
        got[SHA256_HEX] = 0;

        const char *want =
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        if (strcmp(got, want) == 0) {
            puts("  ok    1,000,000 x 'a' (streaming, 1000-byte chunks)");
        } else {
            printf("  FAIL  1,000,000 x 'a'\n        expected %s\n"
                   "        got      %s\n", want, got);
            failures++;
        }
    }

    if (failures) {
        printf("\n%d vector(s) FAILED\n", failures);
        return 1;
    }
    puts("\nAll 4 NIST vectors pass.");
    return 0;
}
