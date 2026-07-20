/* commands.h — the six user-facing commands.
 *
 * Every one of these takes the arguments AFTER the command name, so
 * `mygit commit -m "x"` reaches cmd_commit as argc=2, argv={"-m", "x"}.
 * Each returns the process exit status: 0 on success.
 *
 * With the exception of cmd_init (which creates the repository), every
 * command calls repo_find_and_chdir() first, so all of them run with the
 * current directory at the repository root and can treat every path as
 * relative to it.
 */
#ifndef MYGIT_COMMANDS_H
#define MYGIT_COMMANDS_H

/* Create .mygit/ in the current directory. */
int cmd_init(int argc, char **argv);

/* Hash files into blobs and record them in the index. */
int cmd_add(int argc, char **argv);

/* Turn the index into a tree + commit, and advance the branch. */
int cmd_commit(int argc, char **argv);

/* Walk the parent chain from HEAD and print it. */
int cmd_log(int argc, char **argv);

/* Report HEAD vs index vs working tree. */
int cmd_status(int argc, char **argv);

/* Make the working tree match a branch or commit. */
int cmd_checkout(int argc, char **argv);

#endif
