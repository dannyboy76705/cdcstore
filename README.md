# cdcstore

A repository of files cut into chunks, with an index. It prepares data for an archiver; it does not compress, deduplicate or pack anything itself. Each program does one job:

| program | job |
|---|---|
| `cdcstore-add` | store files (into a repository, or in place with `-X`) |
| `cdcstore-remove` | take files out; chunks nobody needs are deleted |
| `cdcstore-restore` | restore everything, a folder, a file or wildcards |
| `cdcstore-list` | list the contents, `-s` for statistics, `-k` for the chunk files |
| `cdcstore-verify` | read everything back and check it, `-c DIR` also compares with the originals |
| `cdcstore-mount` | mount the repository read-only (FUSE) |

## Why

A collection that is stored on discs grows and shrinks one item at a time: add one more ISO, take one out, and see whether the set still fits. Rebuilding a big compressed archive for every change is slow, and the size is only known after compressing. With a repository, adding or removing one file is one command, and the data that goes to the archiver is always a plain list of files.

The repeats are found later, by the two tools that are good at it. `sortof-cdcsort` (from the sortof-suite) puts identical and near-identical chunks next to each other, and the container (`tar | zstd`, `zfrag`, DwarFS) compresses the result. Keeping those jobs out of cdcstore keeps it small: a chunk is a plain file, so a repository is also something you can look at, copy and mount without any tool.

## Build

Needs gcc and make. `cdcstore-mount` also needs libfuse3 and pkg-config.

```
apt install build-essential libfuse3-dev pkg-config     # Debian / Ubuntu
make
make install PREFIX=/usr/local
```

`make install` also installs the man pages (for example `man cdcstore`).

## Use

```
cdcstore-add -d REPO /path/to/files...            # store; the originals stay where they are
cdcstore-add -d REPO -                            # paths on stdin, stored in the order given
cdcstore-list REPO                                # what is in it
cdcstore-list -s REPO                             # numbers: files, sizes, unique chunks
cdcstore-restore -o OUT REPO 'GAMES/DOOM*'        # a path, a folder, or a quoted wildcard
cdcstore-remove -n REPO 'old/*'                   # -n shows what would go; without it, it goes
cdcstore-verify -c /path/to/files REPO            # check the chunks, and compare with the originals
cdcstore-mount REPO /mnt/repo                     # fusermount3 -u /mnt/repo to unmount
```

Every program prints its options with `-h`. Paths are stored relative (no leading `/` or `./`, no `..`); `-C DIR` stores them relative to DIR.

### Chunks

Files up to `-s` bytes (default 65536) are one chunk each. Larger files are cut into content-defined chunks of about `-a` bytes (default 16384, rounded to a power of two between 1 KiB and 4 MiB). A chunk never spans two files. `-a` and `-s` only matter for a new repository: an existing one keeps its own.

### In place: `-X`

`cdcstore-add -X DIR` turns DIR into the repository (`DIR/.cdcstore`) and **deletes the originals** once everything has been stored and checked. It prints a notice and asks first; `-y` skips the question, not the notice. Use `-d` unless you mean it.

### The repository

```
REPO/cdcstore.idx                     the index, rewritten atomically after every change
REPO/chunks/bNNNNN/cNNNNNNNNN         every chunk as its own file, raw bytes, 5000 per bucket folder
```

Chunks are numbered in the order they first appear, so the natural order of the chunk files is the order the data was added. Numbers are never reused, so removing a file does not rename any other chunk, and a list you have already sorted or archived stays valid. Nothing is shared between files here: if two files hold the same bytes they have two sets of chunk files.

The index is laid out to be mapped and used as it is: a header with a magic and a checksum, then the chunk table, the entries (sorted by path, so a lookup is a binary search and a folder is followed by everything inside it), the chunk list of every file with its offsets, and the path strings. A read at any offset finds its chunk by binary search. The full layout is at the top of `cdcstore-common.h`.

## To an archive

`cdcstore-list -k` prints the paths of `cdcstore.idx` and then every chunk file, one per line, in the order they were added. That list is the input for a sorter and an archiver:

```
cdcstore-list -k REPO | sortof-cdcsort - > list.txt
zfrag pack -T list.txt -o set.tar.zfrag
# or:  tar -T list.txt -cf - | zstd -9 > set.tar.zst
```

The sortof tools keep `cdcstore.idx` first without sorting or reading it, so no extra step is needed. Give `-k` a pattern to list only the chunks one file or folder uses: `cdcstore-list -k REPO 'GAMES/DOOM*'`.

Anything that restores from the archive needs all the chunk files of the files it restores. Extract the repository (or mount the archive) and run `cdcstore-restore` or `cdcstore-mount` on it; `zfrag mount` does that without extracting, and shows the files the repository holds under `virtual/` (see the zfrag README).

### What the order is worth

The same repositories, as `tar | zstd -9`, with the chunk list in the order the data was added and then through `sortof-cdcsort` (default `-s 50`):

| | files | chunk order | `sortof-cdcsort` |
|---|---|---|---|
| DOS collection | 622,981 | 6,662,700,321 | 6,103,466,864 (8.4% smaller) |
| Disc images | 73,102 | 921,588,375 | 718,117,377 (22.1% smaller) |

DwarFS and zfrag on the same lists are compared in the zfrag README.

## Mounting

`cdcstore-mount [-f] [-d] REPO MOUNTPOINT` shows the files with their stored paths, modes, times and links. A read opens only the chunk files it touches. It needs every chunk file present (a missing one is an error, not wrong bytes). Nothing is cached or decoded: the repository is plain files.

## Questions and answers

**Why make this? Don't other containers exist?**
They do, but they don't fill my need, or they are too slow. zpaq is my model, but its symmetrical compression (decompressing costs as much as compressing) makes mounted use impractical. Others I tried were tied to one ecosystem without a complete feature set: 7-Zip, for example, reads files in but cannot chunk or sort the input for maximum compression. DwarFS also fills the need, but it cannot chunk files, so its history is limited to its window. This was made so the output can be cold storage (xz), a regular archive (zstd, 7z) or zfrag. When sorted with `sortof-cdcsort`, the data is easier for every one of these compressors to work with.

**Why not bundle it into one tool?**
I wanted it to be container agnostic. I made zfrag, but I also wanted cdcstore to be easy to bring into other flows. Even the sorter is optional: zpaq and DwarFS, for example, have their own.

## Thank you

* **[Debian](https://www.debian.org)**: for a solid distribution that does not move, which makes supporting a version for years instead of days possible.
* **Wen Xia and colleagues**, "FastCDC: a fast and efficient content-defined chunking approach for data deduplication", USENIX ATC 2016: the gear-hash chunker with normalized cut points that `cdcstore-add` is built on.
* **Athicha Muthitacharoen, Benjie Chen and David Mazières**, "A low-bandwidth network file system", SOSP 2001: the idea of cutting files at positions chosen by their content (they used Rabin fingerprints; Michael O. Rabin, "Fingerprinting by random polynomials", 1981).
* **The [libfuse](https://github.com/libfuse/libfuse) team**: for helping turn a ton of chunks into an easy-to-use directory structure.

## License

MIT. Copyright (c) 2026 Daniel Lee Witzel. See `LICENSE`.

### Third-party libraries

`cdcstore-mount` links to libfuse3 (LGPL-2.1). The other programs use no library. Nothing of libfuse is included: it uses the copy installed on the system.

If you package or distribute binaries: with dynamic linking to the system libraries (as a Debian package does) nothing more is needed beyond shipping the `LICENSE` file. If you link a library statically or bundle it, you must also include that library's own license text, and for libfuse (LGPL) let users replace it with their own build.
