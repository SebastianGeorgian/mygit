/* repo.h — repository layout, discovery, and `mygit init`.
 *
 * ON-DISK LAYOUT
 *
 *   .mygit/
 *     HEAD              where we are:  "ref: refs/heads/<branch>"
 *                       or a raw 64-hex commit id (detached HEAD)
 *     index             the staging area (binary — see index.h)
 *     objects/xx/yyyy   the object store (see object.h)
 *     refs/heads/<b>    one file per branch, containing a commit id
 *
 * All paths in this project are RELATIVE to the repository root, and
 * every command calls repo_find_and_chdir() before touching anything.
 * That single convention removes a whole class of bugs: no code below
 * this layer has to care where the user actually ran the command from.
 */
#ifndef MYGIT_REPO_H
#define MYGIT_REPO_H

#define MYGIT_DIR   ".mygit"
#define OBJECTS_DIR ".mygit/objects"
#define REFS_DIR    ".mygit/refs/heads"
#define HEAD_FILE   ".mygit/HEAD"
#define INDEX_FILE  ".mygit/index"

/* The caller's original directory, relative to the repo root ("" at the
 * root). Set by repo_find_and_chdir().
 *
 * This exists so that `mygit add note.txt` run inside sub/ stages
 * "sub/note.txt" and not "note.txt": the prefix is prepended to relative
 * arguments (see norm_path in commands.c). It is a global because it is
 * process-wide state established once at startup, like argv. */
extern char repo_prefix[4096];

/* Walk up from the current directory until a .mygit/ is found, chdir()
 * into that root, and record repo_prefix. Dies if there is no repository
 * anywhere up to "/".
 *
 * This is why mygit works from any subdirectory, exactly like real Git. */
void repo_find_and_chdir(void);

/* Create a fresh repository in the current directory. Dies if one exists. */
void repo_init_here(void);

#endif
