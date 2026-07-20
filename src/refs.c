/* refs.c — reading and writing HEAD and branch refs. */
#include "refs.h"
#include "repo.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strip trailing newlines/carriage returns in place.
 * Ref files are text and end with '\n'; \r is handled too so that a file
 * touched on Windows does not turn into a "corrupt ref" error. */
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static void branch_path(const char *name, char out[4352]) {
    snprintf(out, 4352, "%s/%s", REFS_DIR, name);
}

/* Read HEAD and work out which of the two forms it is in.
 *
 * Note the return value carries the MEANING (attached vs detached) while
 * the buffers carry the data — the caller cannot accidentally treat a
 * detached HEAD as a branch, because it never gets a branch name.
 *
 * Anything that is neither form is corruption and is fatal: silently
 * guessing what a mangled HEAD meant would be much worse than stopping. */
int head_get(char branch_out[128], char commit_out[65]) {
    branch_out[0] = 0;
    commit_out[0] = 0;

    size_t n;
    unsigned char *buf = read_file(HEAD_FILE, &n);
    if (!buf) die("cannot read %s", HEAD_FILE);
    chomp((char *)buf);

    /* Form 1: "ref: refs/heads/<name>" */
    if (strncmp((char *)buf, "ref: refs/heads/", 16) == 0) {
        snprintf(branch_out, 128, "%s", (char *)buf + 16);
        free(buf);
        /* The tip may not exist yet (unborn branch): commit_out simply
         * stays "", and the return value still says "attached". */
        branch_read(branch_out, commit_out);
        return 1;
    }

    /* Form 2: a bare 64-char hash (detached). Validated before being
     * accepted — HEAD is a file, and files can contain anything. */
    if (strlen((char *)buf) == 64 && is_hex((char *)buf, 64)) {
        memcpy(commit_out, buf, 65);
        free(buf);
        return 0;
    }

    free(buf);
    die("corrupt %s", HEAD_FILE);
}

/* "Give me the current commit, I do not care how HEAD stores it."
 * The empty string is the sentinel for "no commits yet", which is why
 * head_get zeroes both buffers up front. */
int head_commit(char out[65]) {
    char branch[128];
    head_get(branch, out);
    return out[0] ? 0 : -1;
}

/* Attach HEAD to a branch. Written atomically like everything else:
 * a corrupt HEAD would make the repository unusable. */
void head_set_branch(const char *name) {
    char line[256];
    int n = snprintf(line, sizeof line, "ref: refs/heads/%s\n", name);
    if (write_file_atomic(HEAD_FILE, line, (size_t)n, 0644) != 0)
        die("cannot update HEAD");
}

/* Detach HEAD onto a specific commit. */
void head_set_detached(const char *hex) {
    char line[80];
    int n = snprintf(line, sizeof line, "%s\n", hex);
    if (write_file_atomic(HEAD_FILE, line, (size_t)n, 0644) != 0)
        die("cannot update HEAD");
}

/* Read a branch tip.
 *
 * Two different failures, treated differently on purpose:
 *   missing file      -> return -1. NORMAL: the branch is unborn.
 *   file with garbage -> die. NOT normal: the repository is damaged and
 *                        continuing would corrupt it further. */
int branch_read(const char *name, char hex[65]) {
    hex[0] = 0;

    char path[4352];
    branch_path(name, path);

    size_t n;
    unsigned char *buf = read_file(path, &n);
    if (!buf) return -1;                        /* unborn branch */
    chomp((char *)buf);

    if (strlen((char *)buf) != 64 || !is_hex((char *)buf, 64)) {
        free(buf);
        die("corrupt ref refs/heads/%s", name);
    }
    memcpy(hex, buf, 65);
    free(buf);
    return 0;
}

/* Create or move a branch. This IS "creating a branch": 65 bytes. */
void branch_write(const char *name, const char *hex) {
    char path[4352];
    branch_path(name, path);

    char line[80];
    int n = snprintf(line, sizeof line, "%s\n", hex);
    if (write_file_atomic(path, line, (size_t)n, 0644) != 0)
        die("cannot update refs/heads/%s", name);
}

int branch_exists(const char *name) {
    char path[4352];
    branch_path(name, path);
    return path_exists(path);
}
