/* tree.h — tree objects: a snapshot of one directory level.
 *
 * PAYLOAD FORMAT (real Git's shape, with 32-byte SHA-256 hashes):
 *
 *   for each entry, concatenated with no separator:
 *       "<octal mode> <name>\0" + raw hash (32 bytes)
 *
 *   mode 100644  regular file
 *   mode 100755  executable file
 *   mode  40000  subdirectory (the hash is another tree object)
 *
 * A tree does not know its own name or where it lives — its parent knows.
 * That is what makes an identical subdirectory appearing in two places
 * cost exactly one object.
 *
 * THE RECURSION, MADE CONCRETE
 * A root tree containing a file and a directory looks like this on disk:
 *
 *   tree 87\0 100644 a.txt\0 <32B hash of the blob>
 *             40000  lib\0   <32B hash of ANOTHER TREE OBJECT>
 *                            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *   and those 32 bytes are literally the filename of that object under
 *   .mygit/objects/. That is the whole of Git in one line: a tree of
 *   nodes that reference their children by the hash of their content.
 *
 * THE TWO DIRECTIONS
 *   tree_build_from_index  flat sorted index -> hierarchy of trees
 *                          (used by commit)
 *   tree_flatten           hierarchy of trees -> flat list
 *                          (used by status and checkout)
 */
#ifndef MYGIT_TREE_H
#define MYGIT_TREE_H

#include "index.h"

#include <stdint.h>

/* One file, as seen from a flattened tree. */
typedef struct {
    char    *path;          /* owned; full path from the repo root */
    char     hash[65];      /* blob id, hex                        */
    uint32_t mode;
} tree_file;

typedef struct {
    tree_file *f;
    size_t n, cap;
} tree_flat;

/* Build the tree hierarchy from the index and return the root id.
 * An empty index yields the (perfectly valid) empty tree. */
void tree_build_from_index(const index_t *ix, char root_hex[65]);

/* Recursively expand a tree into a flat list of files.
 * The result is sorted by path, because the trees were built from sorted
 * input and are walked in order. */
void tree_flatten(const char *tree_hex, tree_flat *out);

/* Linear search by path. O(n) — see the note in tree.c. */
tree_file *tree_flat_find(const tree_flat *t, const char *path);

void tree_flat_free(tree_flat *t);

#endif
