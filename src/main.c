/* mygit — a minimal Git, written from scratch in C.
 *
 * WHAT THIS IS
 * A working subset of Git — init, add, commit, log, status, checkout —
 * built on the same core model the real thing uses: a content-addressable
 * object store of blobs, trees and commits (object.h), a binary staging
 * area (index.h), and refs (refs.h). No external libraries, including
 * SHA-256, which is implemented from scratch in sha256.c.
 *
 * WHERE TO START READING
 * Not alphabetically, and not with commands.c — that is the biggest file
 * and it is only orchestration. This order builds up:
 *
 *   1. main.c      you are here: the map
 *   2. util.h      the vocabulary every module speaks
 *   3. sha256.h/c  self-contained, no dependencies
 *   4. object.h/c  THE CORE IDEA — read this one twice
 *   5. index.h/c   binary serialization
 *   6. tree.h/c    the algorithm worth understanding
 *   7. commit.c, refs.c   easy once objects make sense
 *   8. commands.c  where it all comes together
 *
 * This file itself is deliberately tiny: parse the command name, dispatch,
 * get out of the way. All the real work lives behind commands.h.
 */
#include "commands.h"

#include <stdio.h>
#include <string.h>

/* Print usage to stderr and return exit code 2.
 *
 * stderr, not stdout, so that `mygit log > file` on a mistyped command
 * does not silently fill the file with help text. Exit code 2 rather than
 * 1 follows the convention that 1 means "ran and failed" while 2 means
 * "you used it wrong" — the shell and CI can tell them apart. */
static int usage(void) {
    fputs(
        "usage: mygit <command> [args]\n"
        "\n"
        "commands:\n"
        "  init                  create an empty repository (.mygit)\n"
        "  add <path>...         stage files or directories ('.' for everything;\n"
        "                        also stages deletions under the given paths)\n"
        "  commit -m <message>   record the staged snapshot\n"
        "  status                show staged / unstaged / untracked changes\n"
        "  log                   show the commit history from HEAD\n"
        "  checkout [-f] <t>     switch to a branch or commit\n"
        "                        (t = branch name, or a hash prefix of >= 4 chars)\n",
        stderr);
    return 2;
}

/* Dispatch on argv[1] and hand the rest of the arguments to the command.
 *
 * The argv + 2 / argc - 2 shift means each command sees only its own
 * arguments and never has to know about the program name or its own name.
 * There is no getopt here on purpose: with six commands and two flags,
 * hand-parsing is shorter and clearer than the configuration it would
 * take to set getopt_long up. */
int main(int argc, char **argv) {
    if (argc < 2) return usage();

    const char *cmd = argv[1];
    int n = argc - 2;
    char **args = argv + 2;

    if (strcmp(cmd, "init") == 0)     return cmd_init(n, args);
    if (strcmp(cmd, "add") == 0)      return cmd_add(n, args);
    if (strcmp(cmd, "commit") == 0)   return cmd_commit(n, args);
    if (strcmp(cmd, "log") == 0)      return cmd_log(n, args);
    if (strcmp(cmd, "status") == 0)   return cmd_status(n, args);
    if (strcmp(cmd, "checkout") == 0) return cmd_checkout(n, args);

    fprintf(stderr, "mygit: '%s' is not a mygit command\n\n", cmd);
    return usage();
}
