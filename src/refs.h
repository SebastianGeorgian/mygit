/* refs.h — HEAD and branches: where we are, and what the names point to.
 *
 * .mygit/HEAD holds ONE of two forms:
 *
 *   "ref: refs/heads/master\n"   -> attached: we are on a branch
 *   "<64-hex commit id>\n"       -> detached: we are on a commit
 *
 * .mygit/refs/heads/<branch> holds that branch's tip commit id.
 *
 * WHY THE INDIRECTION?
 * So that `commit` can advance the branch automatically. On master:
 *   1. write the new commit object
 *   2. update refs/heads/master to the new id
 *   3. HEAD is UNCHANGED — it still says "ref: refs/heads/master"
 * HEAD points at a NAME; the name follows the history.
 *
 * And that is the whole secret of branches: a branch is a file with 64
 * characters in it. That is why creating one in Git is instant — it
 * writes 65 bytes. It is also why "detached HEAD" is not an error: it
 * just means HEAD holds an id directly, so new commits advance no name.
 *
 * UNBORN BRANCHES
 * Right after init, HEAD says "ref: refs/heads/master" but that file does
 * not exist yet. That is the correct representation of "on a branch, no
 * commits yet" — not a bug, and every function here handles it.
 */
#ifndef MYGIT_REFS_H
#define MYGIT_REFS_H

/* Read HEAD.
 * Returns 1 if attached: branch_out gets the name, commit_out gets the
 *           tip — or "" if the branch is unborn.
 * Returns 0 if detached: commit_out gets the id, branch_out is "". */
int head_get(char branch_out[128], char commit_out[65]);

/* Resolve HEAD to a commit id, whichever form it is in.
 * Returns 0 with `out` filled, or -1 if there are no commits yet
 * (out is set to ""). The -1 is the standard "is this repo empty?" test. */
int head_commit(char out[65]);

/* Point HEAD at a branch (attach). */
void head_set_branch(const char *name);

/* Point HEAD directly at a commit (detach). */
void head_set_detached(const char *hex);

/* Read a branch's tip. Returns 0 on success, -1 if the ref file is
 * missing (unborn branch). Dies if the file exists but is not a hash. */
int branch_read(const char *name, char hex[65]);

/* Create or move a branch to `hex`. */
void branch_write(const char *name, const char *hex);

/* True if the ref file exists. Used by checkout to tell a branch name
 * from a hash prefix. */
int branch_exists(const char *name);

#endif
