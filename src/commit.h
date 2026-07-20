/* commit.h — commit objects: a snapshot plus its ancestry.
 *
 * PAYLOAD (plain text):
 *
 *   tree <64-hex>\n
 *   parent <64-hex>\n          <- absent on the very first commit
 *   author <name> <unix-ts>\n
 *   \n                         <- blank line: end of headers
 *   <message>\n
 *
 * WHY TEXT, WHEN THE INDEX IS BINARY?
 * Different files, different criteria — this is a design decision, not an
 * inconsistency:
 *
 *                   index                    commit
 *   written         thousands of times/day   once, then only read
 *   size            can reach megabytes      ~200 bytes
 *   read by humans  never                    yes, when debugging
 *   choice          BINARY (speed, size)     TEXT (legibility)
 *
 * WHAT A COMMIT IS, STRUCTURALLY
 * A commit is not a diff. It is a pointer to a complete snapshot (the
 * tree) plus a pointer to the previous commit. Following `parent`
 * repeatedly walks the history — which is all `log` does.
 *
 * Real Git allows several parents (that is what a merge is), so its
 * history is a DAG. Here every commit has at most one, so it is a chain.
 * Merging is the natural extension, and it is where the interesting
 * algorithms live (finding the common ancestor, three-way merge).
 */
#ifndef MYGIT_COMMIT_H
#define MYGIT_COMMIT_H

#include <time.h>

typedef struct {
    char   tree[65];        /* the snapshot this commit points at    */
    char   parent[65];      /* "" for the root commit                */
    char   author[256];
    time_t when;            /* unix timestamp                        */
    char  *message;         /* malloc'd — CALLER FREES               */
} commit_info;

/* Create and store a commit object, filling out_hex.
 * `parent_hex` may be NULL or "" for the first commit. */
void commit_create(const char *tree_hex, const char *parent_hex,
                   const char *message, char out_hex[65]);

/* Parse a commit object. Dies if the id is not a commit.
 * The caller must free ci->message. */
void commit_parse(const char *hex, commit_info *ci);

#endif
