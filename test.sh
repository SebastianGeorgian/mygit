#!/bin/sh
# test.sh — end-to-end test suite for mygit.
# Exercises: init, add (files/dirs/deletions), commit, log, status,
# checkout (branch + abbreviated detached), binary round-trips,
# nested directories, and the dirty-checkout safety guard.
set -eu

BIN="$(pwd)/mygit"
[ -x "$BIN" ] || { echo "build first: make" >&2; exit 1; }

TMP=$(mktemp -d)
KEEP="$TMP/.keep"; mkdir -p "$KEEP"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

say()  { printf '== %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# ── init ─────────────────────────────────────────────────────
say "init"
"$BIN" init | grep -q "Initialized" || fail "init message"
[ -d .mygit/objects ] && [ -d .mygit/refs/heads ] || fail "repo layout"
if "$BIN" init >/dev/null 2>&1; then fail "double init not refused"; fi

# ── working tree: text, nested dirs, binary ──────────────────
say "populate working tree"
mkdir -p src docs/deep
printf 'hello\n' > a.txt
printf 'int main(void){return 0;}\n' > src/main.c
head -c 4096 /dev/urandom > docs/deep/blob.bin
cp docs/deep/blob.bin "$KEEP/blob.orig"

say "status: untracked"
OUT=$("$BIN" status)
echo "$OUT" | grep -q "No commits yet"       || fail "no-commits banner"
echo "$OUT" | grep -q "Untracked files"      || fail "untracked section"
echo "$OUT" | grep -q "docs/deep/blob.bin"   || fail "untracked nested file"

# ── add + staged status ──────────────────────────────────────
say "add ."
"$BIN" add .
OUT=$("$BIN" status)
echo "$OUT" | grep -q "new file:   a.txt"        || fail "staged new a.txt"
echo "$OUT" | grep -q "new file:   src/main.c"   || fail "staged new src/main.c"
if echo "$OUT" | grep -q "Untracked"; then fail "still untracked after add"; fi

# ── commit 1 ─────────────────────────────────────────────────
say "commit 1"
"$BIN" commit -m "first commit" | grep -q "first commit" || fail "commit 1"
"$BIN" status | grep -q "working tree clean" || fail "clean after commit 1"
if "$BIN" commit -m "again" >/dev/null 2>&1; then fail "empty commit not refused"; fi

say "log 1"
"$BIN" log | grep -q "first commit" || fail "log shows commit 1"
"$BIN" log | grep -q "HEAD -> master" || fail "log decoration"

# ── modify, unstaged status, commit 2 ────────────────────────
say "modify + unstaged"
printf 'hello v2, longer content\n' > a.txt
OUT=$("$BIN" status)
echo "$OUT" | grep -q "Changes not staged"   || fail "unstaged section"
echo "$OUT" | grep -q "modified:   a.txt"    || fail "unstaged modified"

say "stage + commit 2"
"$BIN" add a.txt
"$BIN" commit -m "second commit" >/dev/null
"$BIN" log | grep -q "second commit" || fail "log shows commit 2"
[ "$("$BIN" log | grep -c '^commit ')" = "2" ] || fail "two commits in log"

C1=$("$BIN" log | awk '/^commit /{h=$2} END{print h}')   # oldest commit
SHORT=$(printf %s "$C1" | cut -c1-10)

# ── detached checkout via abbreviated hash ───────────────────
say "checkout $SHORT (detached, abbreviated)"
"$BIN" checkout "$SHORT" | grep -q "HEAD is now at" || fail "detached checkout"
grep -qx 'hello' a.txt                         || fail "a.txt reverted"
cmp -s docs/deep/blob.bin "$KEEP/blob.orig"    || fail "binary round-trip"
"$BIN" status | grep -q "HEAD detached"        || fail "detached status"
"$BIN" status | grep -q "working tree clean"   || fail "clean when detached"

say "checkout master (forward)"
"$BIN" checkout master | grep -q "Switched to branch 'master'" || fail "back to master"
grep -q 'hello v2' a.txt || fail "a.txt forward content"

# ── deletion staging and restore ─────────────────────────────
say "deletions"
rm src/main.c
"$BIN" status | grep -q "deleted:    src/main.c" || fail "unstaged deletion"
"$BIN" add .
OUT=$("$BIN" status)
echo "$OUT" | grep -q "Changes to be committed"   || fail "deletion staged section"
echo "$OUT" | grep -q "deleted:    src/main.c"    || fail "deletion staged entry"
if echo "$OUT" | grep -q "Changes not staged"; then fail "deletion still unstaged"; fi
"$BIN" commit -m "remove main.c" >/dev/null

"$BIN" checkout "$SHORT" >/dev/null
[ -f src/main.c ] || fail "deleted file restored on checkout"
"$BIN" checkout master >/dev/null
[ ! -e src/main.c ] || fail "file removed when switching forward"
[ ! -d src ]        || fail "empty dir cleaned up"

# ── dirty checkout guard ─────────────────────────────────────
say "dirty checkout refused / forced"
printf 'dirty edit\n' >> a.txt
if "$BIN" checkout "$SHORT" >/dev/null 2>&1; then fail "dirty checkout not refused"; fi
"$BIN" checkout -f "$SHORT" >/dev/null
grep -qx 'hello' a.txt || fail "forced checkout content"
"$BIN" checkout master >/dev/null

# ── subdirectory invocation ──────────────────────────────────
say "running from a subdirectory"
mkdir -p sub
printf 'from subdir\n' > sub/note.txt
cd sub
"$BIN" add note.txt
cd ..
"$BIN" status | grep -q "new file:   sub/note.txt" || fail "prefix-relative add"
"$BIN" commit -m "note from subdir" >/dev/null

# ── object store sanity ──────────────────────────────────────
say "object store layout"
NOBJ=$(find .mygit/objects -type f | wc -l)
[ "$NOBJ" -ge 10 ] || fail "expected >= 10 objects, got $NOBJ"
find .mygit/objects -type f | awk -F/ '{ if (length($(NF-1)) != 2 || length($NF) != 62) exit 1 }' \
    || fail "object path shape"

echo
echo "ALL TESTS PASSED"
