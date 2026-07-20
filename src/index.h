/* index.h — the staging area.
 *
 * WHY AN INDEX EXISTS AT ALL
 * The question every newcomer asks: why `add` *and* `commit`? Because
 * there are three states, not two:
 *
 *   HEAD (last commit) <--staged-- INDEX <--unstaged-- WORKING TREE
 *      what is saved            what will be         what is on disk
 *                                 saved next
 *
 * The index is what lets you touch five files and commit two of them —
 * a clean, coherent commit with a single purpose. Without it, every
 * commit would be "everything I happened to have changed".
 *
 * `status` is literally a report of those two comparisons, and nothing
 * else (see changes_collect in commands.c).
 *
 * ON-DISK FORMAT (.mygit/index) — all integers BIG-ENDIAN:
 *
 *   +--------+---------+---------+
 *   | "MGIX" | ver u32 | cnt u32 |            header
 *   +--------+---------+---------+
 *   | mode u32 | mtime u64 | size u64 |       one per entry,
 *   | hash raw[32] | pathlen u16 | path |     sorted by path
 *   +----------------------------------+
 *
 *   "MGIX"    magic number. If the first four bytes are not this, the
 *             file is not an index and we say so instead of interpreting
 *             garbage. (PNG starts with \x89PNG, ZIP with PK, ELF with
 *             \x7fELF — same idea.)
 *   ver       lets the format evolve: add a field tomorrow, bump the
 *             version, and old binaries refuse politely instead of
 *             misreading.
 *   cnt       how many entries follow, so the reader never has to guess.
 *   pathlen   LENGTH-PREFIXING: the length comes before the variable-sized
 *             data, so the reader knows exactly how much to take. A
 *             terminator would not work — paths could contain it.
 *
 * The hash is stored RAW (32 bytes) rather than hex (64): half the size
 * on disk. In memory hex is more convenient (printable, strcmp-able), so
 * the conversion happens at the boundary.
 *
 * INVARIANT, RELIED ON ELSEWHERE: entries are ALWAYS sorted by path with
 * strcmp. index_find binary-searches on it, and tree.c's whole algorithm
 * depends on entries of one directory being contiguous. Any code that
 * mutates the index must preserve the ordering.
 */
#ifndef MYGIT_INDEX_H
#define MYGIT_INDEX_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t mode;          /* 0100644 (normal) or 0100755 (executable).
                             * Only the exec bit is tracked, like Git —
                             * the rest of the permissions are the user's
                             * business, not the repository's.           */
    uint64_t mtime;         /* modification time, seconds                */
    uint64_t size;          /* file size in bytes                        */
    char     hash[65];      /* blob id, hex + NUL                        */
    char    *path;          /* owned; relative to the repo root, '/'     */
} idx_entry;

/* mtime and size are cached here purely as a fast "has this changed?"
 * test: if stat() reports the same size, mtime and mode, status skips
 * re-hashing the file. That is the reason `git status` is instant on a
 * huge repository, and it is a deliberate trade — a change that preserves
 * all three is missed. Real Git has the same limitation. */

typedef struct {
    idx_entry *e;
    size_t n, cap;
} index_t;

/* Load the index. A missing file is NOT an error: it means "empty",
 * which is exactly the state right after init. */
void index_load(index_t *ix);

/* Serialize and write the index atomically. */
void index_save(const index_t *ix);

/* Binary-search for a path. Returns NULL if absent.
 * The pointer is invalidated by any subsequent index_set/index_remove. */
idx_entry *index_find(const index_t *ix, const char *path);

/* Insert or update an entry, preserving the sort order. */
void index_set(index_t *ix, const char *path, const char *hash,
               uint32_t mode, uint64_t mtime, uint64_t size);

/* Remove an entry. Returns 1 if it was there, 0 if not. */
int index_remove(index_t *ix, const char *path);

/* Free every entry and the array. */
void index_free(index_t *ix);

#endif
