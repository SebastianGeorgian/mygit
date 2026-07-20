/* commands.c — the six commands, and the helpers they share.
 *
 * This is the orchestration layer: it does no hashing, no serialization
 * and no tree building itself — it calls object.c, index.c, tree.c and
 * refs.c and arranges the results into what a user asked for.
 *
 * Read it LAST. Every interesting decision lives in the modules below it;
 * what is interesting here is the ordering of operations (particularly in
 * checkout) and the safety checks.
 *
 * LAYOUT
 *   helpers      path normalization, directory walking, change detection
 *   init/add     staging
 *   commit/log   history
 *   status       the three-way report
 *   checkout     the only command that destroys anything
 */
#include "commands.h"
#include "commit.h"
#include "index.h"
#include "object.h"
#include "refs.h"
#include "repo.h"
#include "tree.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ════════════════════════════════ helpers ═══════════════════════════ */

/* Turn a user-supplied path argument into a repo-root-relative path.
 *
 * WHAT IT HANDLES
 *   - repo_prefix: `mygit add note.txt` inside sub/ must stage
 *     "sub/note.txt". The caller's subdirectory is prepended here, once,
 *     so no other code has to think about it.
 *   - "." means "everything under here" — at the root that is the empty
 *     string, which every caller reads as "the whole repository".
 *   - "./x" and "x/" and "x" must all mean the same thing.
 *
 * WHAT IT REJECTS, AND WHY IT MATTERS
 *   - absolute paths: they are not inside the repository.
 *   - any ".." component: PATH TRAVERSAL. Without this check,
 *     `mygit add ../../etc/passwd` would happily hash files outside the
 *     repository into the object store. This is the same class of bug
 *     that produces the endless "../../../etc/passwd" CVEs in web
 *     servers. It is not critical here, but validating untrusted input at
 *     the boundary is the reflex worth having.
 *
 * `tmp` is deliberately larger than `out`: the prefix is prepended before
 * anything is trimmed, so the intermediate can legitimately be longer
 * than the final result. The %.4095s on the way out caps it. */
static void norm_path(const char *arg, char out[4096]) {
    if (arg[0] == '/')
        die("absolute paths are not supported: %s", arg);

    char tmp[8192];
    int is_dot = strcmp(arg, ".") == 0;

    /* Four cases: {in a subdir, at the root} x {".", a real path}. */
    if (repo_prefix[0] && is_dot)
        snprintf(tmp, sizeof tmp, "%s", repo_prefix);       /* "." in sub/ */
    else if (repo_prefix[0])
        snprintf(tmp, sizeof tmp, "%s/%s", repo_prefix, arg);
    else if (is_dot)
        tmp[0] = 0;                                         /* "." at root */
    else
        snprintf(tmp, sizeof tmp, "%s", arg);

    /* Strip leading "./" (possibly repeated: "././x"). */
    char *p = tmp;
    while (p[0] == '.' && p[1] == '/') p += 2;

    /* Strip trailing slashes so "dir/" and "dir" are the same path. */
    size_t n = strlen(p);
    while (n && p[n - 1] == '/') p[--n] = 0;

    /* Walk the components and reject "..".
     * Checking for the SUBSTRING ".." would be wrong — it would reject
     * the legitimate filename "my..file". Only a whole component counts. */
    const char *q = p;
    while (*q) {
        const char *seg_end = strchr(q, '/');
        size_t seg = seg_end ? (size_t)(seg_end - q) : strlen(q);
        if (seg == 2 && q[0] == '.' && q[1] == '.')
            die("path may not contain '..': %s", arg);
        q += seg;
        if (*q == '/') q++;
    }

    snprintf(out, 4096, "%.4095s", p);
}

/* Callback type for walk_files: called once per regular file found.
 *
 * `ud` ("user data") is how arbitrary context reaches the callback — this
 * is the C equivalent of a closure. Together they let ONE traversal serve
 * completely different jobs: `add` passes stage_file, `status` passes
 * collect_untracked. Without the callback, walk_files would exist twice. */
typedef void (*walk_cb)(const char *path, const struct stat *st, void *ud);

/* Recursively walk the working tree under `dir` ("" = repo root).
 *
 * THREE THINGS THAT MUST BE RIGHT:
 *
 * 1. Skipping "." and "..". Forget it and you recurse into the current
 *    directory forever — the classic first bug in every directory walker.
 *
 * 2. Skipping .mygit. The repository must not version itself.
 *
 * 3. lstat, NOT stat. stat follows symlinks; lstat reports on the link
 *    itself. With stat, a symlink pointing at a parent directory would
 *    look like a directory and recursion would never terminate. Symlinks
 *    are simply ignored here (a documented limitation), and that is what
 *    makes it safe. */
static void walk_files(const char *dir, walk_cb cb, void *ud) {
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return;                     /* unreadable: skip, do not fail */

    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (strcmp(name, MYGIT_DIR) == 0) continue;

        char path[4096];
        if (snprintf(path, sizeof path, "%s%s%s",
                     dir, *dir ? "/" : "", name) >= (int)sizeof path)
            die("path too long under %s", dir);

        struct stat st;
        if (lstat(path, &st) != 0) continue;    /* vanished mid-walk: skip */

        if (S_ISDIR(st.st_mode)) walk_files(path, cb, ud);
        else if (S_ISREG(st.st_mode)) cb(path, &st, ud);
        /* symlinks, devices, fifos: deliberately ignored */
    }
    closedir(d);
}

/* Hash a working-tree file exactly as it would be stored.
 *
 * The "blob" type is what makes this comparable to what is in the index:
 * the object id covers the header too, so hashing the bare contents would
 * produce a different value and every comparison would fail. */
static int hash_worktree_file(const char *path, char hex[65]) {
    size_t n;
    unsigned char *data = read_file(path, &n);
    if (!data) return -1;
    obj_hash("blob", data, n, hex);
    free(data);
    return 0;
}

/* Reduce a file's permissions to the only bit that is tracked.
 *
 * Git records exec/not-exec and nothing else: the rest of the mode is the
 * user's local business (and varies with umask), so tracking it would
 * produce spurious differences between machines. S_IXUSR — the owner's
 * execute bit — is the one that decides. */
static uint32_t mode_of(const struct stat *st) {
    return (st->st_mode & S_IXUSR) ? 0100755u : 0100644u;
}

/* Flatten HEAD's tree into `flat`.
 * Returns 1 if HEAD has a commit, 0 if the repository has none yet —
 * in which case `flat` is a valid empty list, so callers can compare
 * against it without a special case. */
static int load_head_tree(tree_flat *flat) {
    flat->f = NULL; flat->n = flat->cap = 0;

    char head[65];
    if (head_commit(head) != 0) return 0;       /* no commits yet */

    commit_info ci;
    commit_parse(head, &ci);
    tree_flatten(ci.tree, flat);
    free(ci.message);
    return 1;
}

/* ── change detection ────────────────────────────────────────────────
 *
 * The heart of `status`, and reused by `checkout` for its safety check.
 * Two independent comparisons, which is exactly what the three states
 * described in index.h imply:
 *
 *   staged    = index vs HEAD tree      "Changes to be committed"
 *   unstaged  = working tree vs index   "Changes not staged for commit"
 *
 * A file can legitimately appear in both (staged, then edited again).
 * That is not a bug in the report — it is the truth about three states. */

typedef struct {
    strvec staged_new, staged_mod, staged_del;
    strvec unstaged_mod, unstaged_del;
} changes;

static void changes_collect(const index_t *ix, const tree_flat *head,
                            changes *c) {
    memset(c, 0, sizeof *c);            /* a zeroed strvec is a valid empty one */

    /* ── index vs HEAD ──
     * In the index but not in HEAD          -> new file
     * In both, different hash or mode       -> modified
     * (mode counts: chmod +x is a real change, even with identical bytes) */
    for (size_t i = 0; i < ix->n; i++) {
        tree_file *tf = tree_flat_find(head, ix->e[i].path);
        if (!tf)
            sv_push(&c->staged_new, xstrdup(ix->e[i].path));
        else if (strcmp(tf->hash, ix->e[i].hash) != 0 ||
                 tf->mode != ix->e[i].mode)
            sv_push(&c->staged_mod, xstrdup(ix->e[i].path));
    }
    /* In HEAD but not in the index -> a staged deletion. */
    for (size_t i = 0; i < head->n; i++)
        if (!index_find(ix, head->f[i].path))
            sv_push(&c->staged_del, xstrdup(head->f[i].path));
    /* The index is sorted, so the lists built from it already are; this
     * one comes from the HEAD tree, so it needs an explicit sort to make
     * the output stable. */
    sv_sort(&c->staged_del);

    /* ── working tree vs index ── */
    for (size_t i = 0; i < ix->n; i++) {
        const idx_entry *e = &ix->e[i];
        struct stat st;
        if (stat(e->path, &st) != 0 || !S_ISREG(st.st_mode)) {
            sv_push(&c->unstaged_del, xstrdup(e->path));    /* gone */
            continue;
        }

        /* THE OPTIMIZATION THAT MAKES status FAST.
         * If size, mtime and mode all match what was recorded at add
         * time, assume the contents match too and skip the hash. One
         * stat() (microseconds) instead of reading and hashing the file.
         * On a repository the size of the Linux kernel this is the
         * difference between instant and tens of seconds — it is why
         * `git status` feels free, and real Git does exactly this.
         *
         * The trade-off, consciously accepted: a change that preserves
         * all three (possible with `touch -r`) is missed. Git has the
         * same hole. */
        if ((uint64_t)st.st_size == e->size &&
            (uint64_t)st.st_mtime == e->mtime &&
            mode_of(&st) == e->mode)
            continue;

        /* Stat says "maybe" — now the hash decides. A file can have a new
         * mtime and identical contents (save with no edit), which is why
         * this is not reported as modified without checking. */
        char hex[65];
        if (hash_worktree_file(e->path, hex) != 0 ||
            strcmp(hex, e->hash) != 0 || mode_of(&st) != e->mode)
            sv_push(&c->unstaged_mod, xstrdup(e->path));
    }
}

/* Total number of differences — checkout's "is the tree dirty?" test. */
static size_t changes_total(const changes *c) {
    return c->staged_new.n + c->staged_mod.n + c->staged_del.n +
           c->unstaged_mod.n + c->unstaged_del.n;
}

static void changes_free(changes *c) {
    sv_free(&c->staged_new);  sv_free(&c->staged_mod);
    sv_free(&c->staged_del);  sv_free(&c->unstaged_mod);
    sv_free(&c->unstaged_del);
}

/* Copy the first line of a message (for one-line summaries).
 * Bounded by construction: it stops at 79 characters or the first
 * newline, whichever comes first, and always terminates. */
static void first_line(const char *msg, char out[80]) {
    size_t i = 0;
    while (msg[i] && msg[i] != '\n' && i < 79) { out[i] = msg[i]; i++; }
    out[i] = 0;
}

/* ════════════════════════════════ init ══════════════════════════════
 * The only command that does NOT call repo_find_and_chdir — it is the one
 * that creates the thing the others look for. */

int cmd_init(int argc, char **argv) {
    (void)argc; (void)argv;             /* takes no arguments */
    repo_init_here();
    return 0;
}

/* ════════════════════════════════ add ═══════════════════════════════
 *
 * `add` does two things people rarely separate in their heads:
 *   1. write the file's contents into the object store as a blob
 *   2. record (path -> blob id, mode, mtime, size) in the index
 * The blob is written IMMEDIATELY, at add time — not at commit. That is
 * why `git add` on a huge file takes a moment, and why the content is
 * already safe once it is staged. */

struct add_ctx { index_t *ix; };        /* what stage_file needs via `ud` */

/* walk_files callback: hash one file into the store and index it. */
static void stage_file(const char *path, const struct stat *st, void *ud) {
    struct add_ctx *ctx = ud;

    size_t n;
    unsigned char *data = read_file(path, &n);
    if (!data) die("cannot read %s", path);

    char hex[65];
    obj_write("blob", data, n, hex);    /* a no-op if already stored */
    free(data);

    /* mtime and size are cached here, at the moment of hashing, so that
     * status can later trust them as a fingerprint. */
    index_set(ctx->ix, path, hex, mode_of(st),
              (uint64_t)st->st_mtime, (uint64_t)st->st_size);
}

/* Stage deletions: drop index entries under `path` whose file is gone.
 *
 * This is what makes `mygit add .` handle removals — without it, deleting
 * a file and adding everything would leave the file staged forever, and
 * `commit` would resurrect it.
 *
 * NOTE THE LOOP: index_remove shifts the array left, so the element that
 * was at i+1 is now AT i. Advancing i after a removal would skip it. That
 * is why the increment is inside the else branch and the loop header has
 * none — a mistake here silently misses every second deletion.
 *
 * The `under` test requires either an exact match or a '/' right after
 * the prefix, so that `add dir` does not accidentally match "dir.txt" —
 * the same trap tree.c has to handle. */
static void stage_deletions_under(index_t *ix, const char *path) {
    size_t plen = strlen(path);
    for (size_t i = 0; i < ix->n; ) {
        const char *ep = ix->e[i].path;
        int under = plen == 0 ||                        /* "" = everything */
                    (strncmp(ep, path, plen) == 0 &&
                     (ep[plen] == '/' || ep[plen] == 0));
        if (under && !path_exists(ep)) {
            index_remove(ix, ep);       /* shifts left; do not advance i */
            continue;
        }
        i++;
    }
}

/* mygit add <path>...
 *
 * Each argument is classified and handled:
 *   ""        -> the whole repository
 *   directory -> recurse into it, and stage deletions under it
 *   file      -> stage it
 *   gone, but in the index -> stage its deletion (`mygit add deleted.txt`)
 *   otherwise -> error, naming the argument the USER typed, not the
 *                normalized form, so the message matches what they wrote */
int cmd_add(int argc, char **argv) {
    if (argc == 0)
        die("nothing specified — try: mygit add <path>...  (or: mygit add .)");

    repo_find_and_chdir();

    index_t ix;
    index_load(&ix);
    struct add_ctx ctx = { &ix };

    for (int i = 0; i < argc; i++) {
        char path[4096];
        norm_path(argv[i], path);

        struct stat st;
        if (path[0] == 0) {                         /* whole repository */
            walk_files("", stage_file, &ctx);
            stage_deletions_under(&ix, "");
        } else if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            walk_files(path, stage_file, &ctx);
            stage_deletions_under(&ix, path);
        } else if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            stage_file(path, &st, &ctx);
        } else if (index_find(&ix, path)) {
            index_remove(&ix, path);                /* stage the deletion */
        } else {
            die("pathspec '%s' did not match any files", argv[i]);
        }
    }

    /* Written ONCE, after every argument is processed: the index is saved
     * atomically, so either all of the additions land or none do. */
    index_save(&ix);
    index_free(&ix);
    return 0;
}

/* ════════════════════════════════ commit ════════════════════════════ */

/* mygit commit -m <message>
 *
 * THE SEQUENCE
 *   1. build tree objects from the index          (tree.c)
 *   2. refuse if that tree equals HEAD's          (nothing changed)
 *   3. write the commit object                    (commit.c)
 *   4. advance the branch, or move detached HEAD  (refs.c)
 *
 * Step 2 is worth a look: instead of comparing files, it compares the
 * ROOT TREE HASH to the parent's. Content addressing makes that a single
 * string comparison that covers the entire tree, at any depth. If nothing
 * changed anywhere, the root hash is identical — that is the whole check. */
int cmd_commit(int argc, char **argv) {
    /* Hand-rolled argument parsing: -m and its value, nothing else. */
    const char *msg = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) msg = argv[++i];
        else die("usage: mygit commit -m <message>");
    }
    if (!msg || !*msg) die("usage: mygit commit -m <message>");

    repo_find_and_chdir();

    index_t ix;
    index_load(&ix);
    if (ix.n == 0)
        die("nothing to commit (the index is empty — use `mygit add` first)");

    char tree[65];
    tree_build_from_index(&ix, tree);

    /* Compare against the parent's tree — one hash, whole snapshot. */
    char parent[65] = "";
    if (head_commit(parent) == 0) {
        commit_info pi;
        commit_parse(parent, &pi);
        int same = strcmp(pi.tree, tree) == 0;
        free(pi.message);
        if (same)
            die("nothing to commit (staged snapshot matches HEAD)");
    }

    char newc[65];
    commit_create(tree, parent[0] ? parent : NULL, msg, newc);

    /* Attached: move the branch, HEAD keeps pointing at its name.
     * Detached: move HEAD itself, since there is no name to move. */
    char branch[128], cur[65];
    int on_branch = head_get(branch, cur);
    if (on_branch) branch_write(branch, newc);
    else head_set_detached(newc);

    char fl[80];
    first_line(msg, fl);
    printf("[%s %.10s] %s\n", on_branch ? branch : "detached HEAD", newc, fl);

    index_free(&ix);
    return 0;
}

/* ════════════════════════════════ log ═══════════════════════════════ */

/* mygit log — follow `parent` from HEAD to the root commit.
 *
 * That loop IS the history: there is no list of commits anywhere, no
 * index of them, no database. Each commit names its predecessor, and
 * walking the names produces the log. */
int cmd_log(int argc, char **argv) {
    (void)argc; (void)argv;
    repo_find_and_chdir();

    char cur[65];
    if (head_commit(cur) != 0) {
        /* An empty repository is not a crash — but it is not success
         * either, so this returns 1 rather than dying. */
        fprintf(stderr, "fatal: no commits yet\n");
        return 1;
    }

    char branch[128], tip[65];
    int on_branch = head_get(branch, tip);

    int first = 1;
    while (cur[0]) {                    /* "" parent = root commit reached */
        commit_info ci;
        commit_parse(cur, &ci);

        /* Only the first line gets the HEAD decoration. */
        if (first && on_branch)
            printf("commit %s (HEAD -> %s)\n", cur, branch);
        else if (first)
            printf("commit %s (HEAD, detached)\n", cur);
        else
            printf("commit %s\n", cur);
        first = 0;

        /* localtime + strftime render the stored unix timestamp in the
         * reader's timezone. The stored value stays absolute; only the
         * display is local. */
        char when[64] = "?";
        struct tm *lt = localtime(&ci.when);
        if (lt) strftime(when, sizeof when, "%a %b %e %H:%M:%S %Y", lt);

        printf("Author: %s\nDate:   %s\n\n", ci.author, when);

        /* Print the message indented by four spaces, line by line.
         * "%.*s" takes the length as an argument, which prints a slice
         * without copying it out or modifying the buffer. */
        const char *m = ci.message;
        while (*m) {
            const char *eol = strchr(m, '\n');
            size_t len = eol ? (size_t)(eol - m) : strlen(m);
            printf("    %.*s\n", (int)len, m);
            m += len;
            if (*m == '\n') m++;
        }
        printf("\n");

        memcpy(cur, ci.parent, 65);     /* step to the parent */
        free(ci.message);
    }
    return 0;
}

/* ════════════════════════════════ status ════════════════════════════ */

struct untracked_ctx { const index_t *ix; strvec *out; };

/* walk_files callback: a file the index has never heard of is untracked. */
static void collect_untracked(const char *path, const struct stat *st,
                              void *ud) {
    (void)st;                           /* only the path matters here */
    struct untracked_ctx *c = ud;
    if (!index_find(c->ix, path))
        sv_push(c->out, xstrdup(path));
}

static void print_list(const strvec *s, const char *label) {
    for (size_t i = 0; i < s->n; i++)
        printf("        %s%s\n", label, s->v[i]);
}

/* mygit status — report the three states against each other.
 *
 * All the thinking is in changes_collect; this is presentation. The
 * sections mirror the diagram in index.h exactly:
 *   HEAD <--staged-- INDEX <--unstaged-- WORKING TREE <--?-- untracked */
int cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    repo_find_and_chdir();

    index_t ix;
    index_load(&ix);

    char branch[128], headhex[65];
    int on_branch = head_get(branch, headhex);

    tree_flat head;
    int have_commit = load_head_tree(&head);

    changes ch;
    changes_collect(&ix, &head, &ch);

    strvec untracked = {0};
    struct untracked_ctx uc = { &ix, &untracked };
    walk_files("", collect_untracked, &uc);
    sv_sort(&untracked);                /* readdir order is arbitrary */

    if (on_branch) printf("On branch %s\n", branch);
    else printf("HEAD detached at %.10s\n", headhex);
    if (!have_commit) printf("\nNo commits yet\n");

    /* `printed` tracks whether anything at all was reported, so the
     * "clean" message is only shown when it is actually true. */
    int printed = 0;

    if (ch.staged_new.n || ch.staged_mod.n || ch.staged_del.n) {
        printf("\nChanges to be committed:\n");
        print_list(&ch.staged_new, "new file:   ");
        print_list(&ch.staged_mod, "modified:   ");
        print_list(&ch.staged_del, "deleted:    ");
        printed = 1;
    }

    if (ch.unstaged_mod.n || ch.unstaged_del.n) {
        printf("\nChanges not staged for commit:\n");
        printf("  (use \"mygit add <file>...\" to update what will be committed)\n");
        print_list(&ch.unstaged_mod, "modified:   ");
        print_list(&ch.unstaged_del, "deleted:    ");
        printed = 1;
    }

    if (untracked.n) {
        printf("\nUntracked files:\n");
        printf("  (use \"mygit add <file>...\" to include in what will be committed)\n");
        print_list(&untracked, "");
        printed = 1;
    }

    if (!printed)
        printf("\nnothing to commit, working tree clean\n");

    changes_free(&ch);
    sv_free(&untracked);
    tree_flat_free(&head);
    index_free(&ix);
    return 0;
}

/* ════════════════════════════════ checkout ══════════════════════════ */

/* After deleting a file, remove the directories that are now empty.
 *
 * Walks upward, rmdir'ing each parent, and STOPS at the first failure:
 * rmdir fails on a non-empty directory, and once one level is non-empty
 * every level above it is too. So the failure is the terminating
 * condition, not an error — which is why the return value is used as
 * control flow rather than checked and reported.
 *
 * The MYGIT_DIR guard is a safety belt: never try to remove the
 * repository itself. */
static void remove_empty_parents(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (;;) {
        char *sl = strrchr(tmp, '/');
        if (!sl) break;                 /* no parent left */
        *sl = 0;
        if (strcmp(tmp, MYGIT_DIR) == 0) break;
        if (rmdir(tmp) != 0) break;     /* not empty (or gone): stop */
    }
}

/* mygit checkout [-f] <branch | commit>
 *
 * THE ONLY COMMAND THAT DESTROYS ANYTHING — which is why the order of
 * operations below is deliberate:
 *
 *   1. check for local changes and REFUSE if any (unless -f)
 *   2. resolve the target (branch name, then hash prefix)
 *   3. check no untracked file would be overwritten (unless -f)
 *   4. delete tracked files that the target does not have
 *   5. write every file from the target
 *   6. rebuild the index to match
 *   7. update HEAD
 *
 * Steps 1 and 3 come BEFORE any destruction: refusing after having
 * already deleted half the tree would be worse than not checking at all.
 * Step 1 reuses status's exact logic, so what checkout considers "dirty"
 * is by construction what status shows. */
int cmd_checkout(int argc, char **argv) {
    int force = 0;
    const char *target = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) force = 1;
        else if (!target) target = argv[i];
        else die("usage: mygit checkout [-f] <branch | commit>");
    }
    if (!target) die("usage: mygit checkout [-f] <branch | commit>");

    repo_find_and_chdir();

    index_t ix;
    index_load(&ix);

    /* 1. Refuse to throw away uncommitted work. */
    if (!force) {
        tree_flat head;
        load_head_tree(&head);
        changes ch;
        changes_collect(&ix, &head, &ch);
        size_t total = changes_total(&ch);
        changes_free(&ch);
        tree_flat_free(&head);
        if (total)
            /* The error message names the exact command that overrides
             * it — a refusal the user cannot act on is just an obstacle. */
            die("you have local changes; commit them first "
                "or use: mygit checkout -f %s", target);
    }

    /* 2. Resolve the target. Branch names are tried FIRST, so a branch
     *    called "abcd" wins over a commit starting with abcd — a name the
     *    user chose beats a coincidence. */
    char commit_hex[65];
    int to_branch = 0;
    if (branch_exists(target)) {
        if (branch_read(target, commit_hex) != 0 || !commit_hex[0])
            die("branch '%s' has no commits yet", target);
        to_branch = 1;
    } else {
        int r = obj_resolve_prefix(target, commit_hex);
        /* -2 (ambiguous) gets its own message: "not found" would send the
         * user looking for a typo when they just need more characters. */
        if (r == -2) die("ambiguous object prefix '%s'", target);
        if (r != 0)  die("'%s' is not a branch or a known commit", target);
    }

    commit_info ci;
    commit_parse(commit_hex, &ci);

    tree_flat want;
    tree_flatten(ci.tree, &want);       /* the target snapshot, flattened */

    /* 3. An untracked file in the way is the user's data — mygit has
     *    never seen it, so it cannot be recovered from the object store.
     *    Refuse rather than overwrite. */
    if (!force) {
        for (size_t i = 0; i < want.n; i++) {
            const char *p = want.f[i].path;
            if (!index_find(&ix, p) && path_exists(p))
                die("untracked file '%s' would be overwritten; "
                    "move it or use -f", p);
        }
    }

    /* 4. Delete tracked files the target does not contain.
     *    Only files in the INDEX are touched: anything untracked is left
     *    alone, because it is not ours to delete. */
    for (size_t i = 0; i < ix.n; i++) {
        if (tree_flat_find(&want, ix.e[i].path)) continue;
        unlink(ix.e[i].path);
        remove_empty_parents(ix.e[i].path);
    }

    /* 5 + 6. Materialize the target and build the new index as we go.
     *
     * The index is rebuilt from scratch (nix) rather than patched: after
     * a checkout it must describe the target exactly, and constructing it
     * here — with the mtime/size read back AFTER writing each file — is
     * both simpler and more correct than editing the old one. Getting
     * those stat values from the file we just wrote is what lets the next
     * `status` be instant and accurate. */
    index_t nix = {0};
    for (size_t i = 0; i < want.n; i++) {
        const tree_file *tf = &want.f[i];

        char type[16];
        size_t n;
        unsigned char *data = obj_read(tf->hash, type, &n);
        if (!data || strcmp(type, "blob") != 0)
            die("missing blob %s for %s", tf->hash, tf->path);

        if (mkdirs_for(tf->path) != 0)          /* target dirs may be new */
            die("cannot create directories for %s", tf->path);

        /* Restore the exec bit from the tree: a script checked out stays
         * runnable. */
        mode_t m = tf->mode == 0100755u ? 0755 : 0644;
        if (write_file_atomic(tf->path, data, n, m) != 0)
            die("cannot write %s", tf->path);
        free(data);

        struct stat st;
        if (stat(tf->path, &st) != 0) die("cannot stat %s", tf->path);
        index_set(&nix, tf->path, tf->hash, tf->mode,
                  (uint64_t)st.st_mtime, (uint64_t)st.st_size);
    }
    index_save(&nix);

    /* 7. Update HEAD — attached to a branch, or detached onto a commit. */
    if (to_branch) {
        head_set_branch(target);
        printf("Switched to branch '%s'\n", target);
    } else {
        head_set_detached(commit_hex);
        char fl[80];
        first_line(ci.message, fl);
        /* Say it out loud: a detached HEAD surprises people, and new
         * commits made here advance no branch. */
        printf("Note: switching to a commit leaves HEAD detached.\n");
        printf("HEAD is now at %.10s %s\n", commit_hex, fl);
    }

    free(ci.message);
    tree_flat_free(&want);
    index_free(&nix);
    index_free(&ix);
    return 0;
}
