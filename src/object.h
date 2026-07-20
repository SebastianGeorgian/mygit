/* object.h — the content-addressable object store. THE CORE IDEA.
 *
 * If you only read one header, read this one. Everything else is built on
 * the model described here.
 *
 * THE MODEL
 * Instead of you naming files ("report_final_v2_FINAL.txt"), the name IS
 * the SHA-256 of the contents. The content dictates its own address.
 *
 * Every object is serialized as
 *
 *     "<type> <size>\0<payload>"
 *
 * hashed OVER THAT WHOLE THING (header included), and stored at
 *
 *     .mygit/objects/<hex[0:2]>/<hex[2:]>
 *
 * which is exactly real Git's scheme, minus zlib compression.
 *
 * WHY THE HEADER IS INSIDE THE HASH
 * To bind the type to the content cryptographically. If only the payload
 * were hashed, a blob and a tree holding the same bytes would collide.
 * With the header, "blob 6\0salut\n" and "tree 6\0salut\n" are entirely
 * different names. The size field is a free integrity check on top: read
 * time compares the declared size to the real one.
 *
 * WHY THE PATH IS SPLIT 2 + 62
 * Not aesthetics — filesystem performance. Many filesystems degrade badly
 * with tens of thousands of entries in a single directory. Two hex chars
 * of prefix spread the objects over 256 subdirectories.
 *
 * WHAT THE MODEL BUYS, FOR FREE
 *   deduplication   identical content -> identical name -> stored once,
 *                   whether it appears twice or a thousand times.
 *   immutability    an object cannot change without changing its name,
 *                   so history cannot be quietly rewritten.
 *   verifiability   re-hash an object and compare it to its own filename;
 *                   any bit rot shows up immediately.
 *
 * THE THREE TYPES (built on top of this file)
 *   blob    raw file bytes                                  (commands.c)
 *   tree    one directory level: names -> hashes            (tree.c)
 *   commit  a tree id + a parent id + author + message      (commit.c)
 */
#ifndef MYGIT_OBJECT_H
#define MYGIT_OBJECT_H

#include <stddef.h>

/* Compute an object id without writing anything.
 * Used by `status` to ask "would this file hash to what the index says?"
 * without polluting the store. */
void obj_hash(const char *type, const void *data, size_t n, char hex[65]);

/* Store an object and fill in its id. A no-op if it already exists —
 * which is where deduplication actually happens. */
void obj_write(const char *type, const void *data, size_t n, char hex[65]);

/* Read an object back.
 *
 * Returns a malloc'd payload (CALLER FREES) with one extra NUL past the
 * end that is not counted in *size, and fills type_out with "blob",
 * "tree" or "commit". Returns NULL if the object is missing or corrupt —
 * callers must check, because "not found" is a normal outcome (e.g. a
 * hash typed by the user). */
unsigned char *obj_read(const char *hex, char type_out[16], size_t *size);

/* True if an object with this full id exists in the store. */
int obj_exists(const char *hex);

/* Resolve an abbreviated hash to a full one, so users can type
 * "checkout a3dd102" instead of 64 characters.
 *
 * Returns  0  resolved (hex filled),
 *         -1  no such object (or the prefix is too short / not hex),
 *         -2  AMBIGUOUS: several objects share the prefix.
 *
 * The three-way return matters: -2 must not be reported as "not found",
 * because the fix is different — the user needs to type more characters,
 * not a different hash. Minimum 4 characters, same as Git. */
int obj_resolve_prefix(const char *prefix, char hex[65]);

#endif
