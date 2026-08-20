# mygit — a minimal Git, written from scratch in C

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C11](https://img.shields.io/badge/C-C11-00599C?logo=c)
![No dependencies](https://img.shields.io/badge/dependencies-none-success)

A working subset of Git implemented in ~1,400 lines of C11 (~3,000 with
comments), with **no external libraries** — including the SHA-256 hash
function, which is written from scratch (FIPS 180-4) and verified against the
official NIST test vectors.

```
$ mygit init
$ mygit add .
$ mygit commit -m "first commit"
$ mygit log
$ mygit status
$ mygit checkout <branch | hash-prefix>
```

It is not a toy that shells out to real Git or fakes the behavior with file
copies: it uses the same core model Git does — a **content-addressable object
store** of blobs, trees and commits, a **binary staging area**, and refs —
so `checkout` really does reconstruct any snapshot from hashes alone.

## Features

| Command | What it does |
|---|---|
| `init` | Creates the `.mygit/` repository layout (objects, refs, HEAD). |
| `add <path>...` | Hashes files into blobs and updates the index. Takes files, directories or `.`; recursing into directories also **stages deletions** of files that vanished. Binary files are first-class (everything is byte-exact). |
| `commit -m <msg>` | Builds the tree hierarchy from the index, writes a commit object, advances the branch. Refuses empty commits. |
| `log` | Walks the parent chain from `HEAD`, with `(HEAD -> master)` decoration. |
| `status` | Three-way comparison — HEAD tree vs index vs working tree: staged (`new file` / `modified` / `deleted`), unstaged, and untracked. |
| `checkout [-f] <t>` | Switches to a branch or to any commit by **abbreviated hash** (detached HEAD). Restores files byte-exact (including the executable bit), removes files absent from the target, cleans up empty directories — and **refuses to destroy local changes** unless `-f` is given. |

Works from any subdirectory of the repository (like real Git, it walks
upwards to find the root and interprets relative paths against your cwd).

## Build & test

```
make            # gcc, -std=c11 -Wall -Wextra -Wpedantic, zero warnings
make test       # end-to-end suite: 14 scenarios incl. binary files,
                # nested dirs, deletions, detached HEAD, safety guards
make check-sha  # SHA-256 against the official NIST vectors
make check      # both of the above
make sanitize   # rebuild with ASan+UBSan and run the suite
```

CI runs all of it on every push, on both gcc and clang, with `-Werror`.

Requires a POSIX system (Linux, macOS, WSL) and gcc or clang.

## How it works

### The object store (content addressing)

Every piece of history is an immutable **object**, serialized as

```
<type> <size>\0<payload>
```

hashed with SHA-256 over the *entire* serialized form, and stored at
`.mygit/objects/<hex[0:2]>/<hex[2:]>` — the same scheme real Git uses, minus
zlib compression. Three object types:

```
blob    raw file bytes
tree    one directory level:  "<octal mode> <name>\0" + 32-byte raw hash, repeated
commit  text: tree <hex> / parent <hex> / author <name> <ts> / blank / message
```

Content addressing gives deduplication for free: two identical files — or a
thousand — are stored exactly once, and an object can never change without
changing its name.

### The index (staging area)

`.mygit/index` is a custom binary format, all integers big-endian:

```
+--------+---------+---------+
| "MGIX" | ver u32 | cnt u32 |            header
+--------+---------+---------+
| mode u32 | mtime u64 | size u64 |       per entry, sorted by path
| hash raw[32] | pathlen u16 | path |
+----------------------------------+
```

`mtime`/`size` are kept so `status` can skip re-hashing files whose stat
information is unchanged — the same optimization real Git relies on.

### Building trees from a flat index

The index is a flat sorted list (`docs/deep/blob.bin`, `src/main.c`, ...) but
commits need a *hierarchy* of tree objects. The builder exploits a property
of lexicographic order — all paths sharing a directory prefix `dir/` are
**contiguous** in the sorted list — so one linear scan per level groups the
children, and each directory group is recursed into first, building the
hierarchy bottom-up with no extra data structures.

### Checkout

`checkout` resolves the target (branch name, or an abbreviated object hash of
≥ 4 chars with ambiguity detection), flattens its tree, then makes the working
tree match: deletes tracked files absent from the target, writes every blob
byte-for-byte with the recorded mode, prunes empty directories, rebuilds the
index, and updates `HEAD` (branch ref or detached). Before touching anything
it runs the same diff logic as `status` and refuses if you have uncommitted
changes (`-f` overrides).

## Design decisions & limitations

Deliberately out of scope, to keep the core model sharp: no zlib compression
or packfiles (objects are stored raw), single-parent commits (no
merge/branching UI — though detached checkout of any commit works, and branch
refs are fully supported internally), no `.mygitignore`, no symlink tracking,
seconds-granularity mtime in the index. Each of these is an isolated
extension point, listed below.

## Roadmap

`branch` / `checkout -b` · `diff` (blob-to-blob) · `.mygitignore` ·
zlib-compressed objects · `cat-file` for inspection · packfiles.

## License

MIT — see `LICENSE`.
