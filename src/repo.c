/* repo.c — finding the repository root, and creating a new one. */
#include "repo.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char repo_prefix[4096];

/* Find the repository root by walking upwards, then chdir into it.
 *
 * THE ALGORITHM
 *   cwd = /home/seb/proj/src/deep
 *   is /home/seb/proj/src/deep/.mygit a directory?  no -> go up
 *   is /home/seb/proj/src/.mygit ...                no -> go up
 *   is /home/seb/proj/.mygit ...                    YES -> root found
 *   chdir there; repo_prefix = "src/deep"
 *
 * Walking up is done by truncating the string at the last '/' rather than
 * by chdir("..") in a loop: it needs no filesystem calls, and it keeps the
 * original path intact so the prefix can be computed at the end. */
void repo_find_and_chdir(void) {
    char cur[4096], orig[4096];

    if (!getcwd(cur, sizeof cur)) die("getcwd failed");
    snprintf(orig, sizeof orig, "%s", cur);     /* remember where we started */

    for (;;) {
        /* Build "<cur>/.mygit". The ternary handles cur == "/", which
         * would otherwise produce the double slash "//.mygit". */
        char probe[4352];
        snprintf(probe, sizeof probe, "%s/%s",
                 strcmp(cur, "/") == 0 ? "" : cur, MYGIT_DIR);
        if (is_dir(probe)) break;               /* found it */

        if (strcmp(cur, "/") == 0)              /* reached the top: give up */
            die("not a mygit repository (no %s up to /)", MYGIT_DIR);

        /* Chop the last component. If the last '/' is the very first
         * character we have landed on the root: keep it as "/" rather
         * than truncating to the empty string. */
        char *sl = strrchr(cur, '/');
        if (sl == cur) cur[1] = 0;
        else *sl = 0;
    }

    if (chdir(cur) != 0) die("cannot chdir to repo root %s", cur);

    /* repo_prefix = orig with the root prefix removed.
     *
     * The strncmp guard is a sanity check: orig must actually start with
     * cur, since cur was derived from it. If that somehow fails, an empty
     * prefix is the safe answer (treat the caller as being at the root)
     * rather than producing a nonsense path. */
    size_t rl = strcmp(cur, "/") == 0 ? 0 : strlen(cur);
    if (strncmp(orig, cur, rl) == 0 && (orig[rl] == '/' || orig[rl] == 0)) {
        const char *p = orig + rl;
        while (*p == '/') p++;                  /* drop the separator */
        snprintf(repo_prefix, sizeof repo_prefix, "%s", p);
    } else {
        repo_prefix[0] = 0;
    }
}

/* Create .mygit/ with its subdirectories and an initial HEAD.
 *
 * Note what is NOT created: refs/heads/master does not exist yet, and
 * neither does the index. That is intentional — HEAD points at a branch
 * that has no commits ("unborn"), which is precisely the state a fresh
 * repository is in, and the rest of the code already handles a missing
 * ref file as "no commits yet" (see refs.c). Creating an empty ref file
 * would mean inventing a special case. */
void repo_init_here(void) {
    if (is_dir(MYGIT_DIR))
        die("already a mygit repository (%s exists)", MYGIT_DIR);

    if (mkdir_p(OBJECTS_DIR) != 0 || mkdir_p(REFS_DIR) != 0)
        die("cannot create %s layout", MYGIT_DIR);

    /* sizeof - 1: write the string without its NUL terminator. HEAD is a
     * text file; a stray NUL in it would be corruption. */
    const char head[] = "ref: refs/heads/master\n";
    if (write_file_atomic(HEAD_FILE, head, sizeof head - 1, 0644) != 0)
        die("cannot write %s", HEAD_FILE);

    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    printf("Initialized empty mygit repository in %s/%s/\n", cwd, MYGIT_DIR);
}
