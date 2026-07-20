# mygit — a minimal Git in C. No external dependencies.
#
#   make            build ./mygit
#   make test       build + run the end-to-end suite
#   make check-sha  validate SHA-256 against the NIST vectors
#   make check      run everything
#   make sanitize   rebuild with ASan+UBSan and run the suite
#   make clean      remove build artifacts

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 \
           -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := mygit

# Flags used by `make sanitize`. AddressSanitizer catches buffer overflows,
# use-after-free and leaks; UBSan catches undefined behaviour that a normal
# build silently tolerates.
SANFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O1 -g \
            -fsanitize=address,undefined -fno-omit-frame-pointer \
            -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

# Every object depends on every header: with this few files, the extra
# rebuilds cost nothing and a missed dependency costs a confusing bug.
src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CFLAGS) -c -o $@ $<

# `sh test.sh` rather than `./test.sh` so the suite runs even if the
# executable bit was lost (unpacked from a zip, copied off a FAT drive).
test: $(BIN)
	sh test.sh

check-sha: tests/sha256_test.c src/sha256.c src/util.c
	$(CC) $(CFLAGS) -o /tmp/mygit_sha256_test $^
	/tmp/mygit_sha256_test

check: check-sha test

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(SANFLAGS)"
	ASAN_OPTIONS=detect_leaks=1 sh test.sh
	$(MAKE) clean

clean:
	rm -f $(BIN) src/*.o /tmp/mygit_sha256_test

.PHONY: all test check check-sha sanitize clean
