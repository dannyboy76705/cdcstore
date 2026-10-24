/*
 * cdcstore-common.h - what the cdcstore programs share: the repository format, the
 * chunker, chunk files, path patterns.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 *
 * THE REPOSITORY (a directory)
 *   cdcstore.idx             the index (one file, rewritten atomically after every change)
 *   chunks/bNNNNN/cNNNNNNNNN every unique chunk as its own file, raw bytes, in buckets of 5000.
 *                            Chunks are numbered in the order they first appear, so the natural
 *                            order of the chunk files is the order of the files that were added.
 *                            A chunk belongs to ONE file and never spans two files; nothing is
 *                            shared or deduplicated, compressed or packed here: finding what
 *                            repeats is the job of the sorter and the archiver.
 *                            `cdcstore-list -k REPO` prints the chunk files, one per line, for an
 *                            external sorter (sortof-*) and then tar | zstd, zfrag, dwarfs, ...
 *                            It prints cdcstore.idx first (it goes in the archive too), and the sortof-*
 *                            tools keep it first without sorting or reading it.
 *
 * THE INDEX, laid out so a mount (cdcstore-mount) can map it and use it as it is. Little endian,
 * every section starts on an 8-byte boundary, every table is a plain array of packed records,
 * and the header holds each section's offset and count:
 *   header   magic "CDCSTOR1", version, chunk size (avg), whole-file size limit, bucket size,
 *            section offsets/counts, checksum
 *   chunks   chunk_t[]   hash (to check the file), length, file number `id`. The chunk
 *                        lives in chunks/b(id / bucket)/c(id); ids are never reused, so names stay
 *                        stable when other files are removed
 *   entries  entry_t[]   SORTED BY PATH ('/' sorts before every other byte), so a directory is
 *                        followed by everything inside it. Each entry has its parent entry and
 *                        `end`, the entry after the last one inside it, so
 *                          - lookup of a path is a binary search,
 *                          - listing a directory is: i = dir+1; while (i < dir.end) { use i; i = en[i].end; }
 *   refs     ref_t[]     per file, in order: the file offset where the chunk begins and the chunk
 *                        number, so a read(offset, length) finds its first chunk by binary search
 *   strings  NUL-terminated paths (relative, no leading '/') and symlink targets
 * To read bytes: chunk number -> chunk file -> bytes [0, len).
 */
#ifndef CDCSTORE_COMMON_H
#define CDCSTORE_COMMON_H
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IDX_NAME   "cdcstore.idx"
#define CHUNK_DIR  "chunks"
#define INPLACE_DIR ".cdcstore"
#define DEF_AVG    16384u
#define DEF_WHOLE  65536u
#define DEF_BUCKET 5000u
#define NO_ENTRY   0xFFFFFFFFu

typedef struct { uint64_t a, b; } h128_t;

#pragma pack(push, 1)
typedef struct { uint64_t h0, h1; uint32_t len, id; } chunk_t;      /* id: the number in the chunk file's name; never reused */
typedef struct { uint64_t off; uint32_t chunk; } ref_t;
typedef struct {
    uint64_t path_off, aux_off;     /* aux: symlink target, else 0 */
    uint64_t size;
    int64_t  mtime;
    uint64_t ref_start, fh0, fh1;
    uint32_t mode, nref, parent, end;
    uint8_t  type, pad[7];
} entry_t;
typedef struct {
    char magic[8];
    uint32_t version, avg, whole, bucket, next_id, pad;
    uint64_t nch, nen, nrefs, nstr;
    uint64_t off_ch, off_en, off_refs, off_str;
    uint64_t total, checksum;
} idxhdr_t;
#pragma pack(pop)
enum { T_DEL = 0, T_FILE = 1, T_DIR = 2, T_LINK = 3 };

typedef struct {
    char *dir;
    uint32_t avg, whole, bucket, next_id;  /* whole: files up to this size are one chunk; next_id: the next chunk file number */
    chunk_t *ch;   size_t nch, capch;
    entry_t *en;   size_t nen, capen;
    ref_t *refs;   size_t nrefs, capref;
    char *str;     size_t nstr, capstr;
    size_t nen_loaded;                     /* entries present when loaded (sorted) */
} repo_t;

/* hashing (not cryptographic: for finding identical chunks, not against an attacker) */
h128_t hash128(const void *p, size_t n);
h128_t fh_start(void);
h128_t fh_fold(h128_t acc, h128_t chunkhash, uint32_t len);

/* chunker: content-defined cuts (gear hash, normalized), min avg/4, max avg*8 */
typedef struct { uint32_t min, avg, max; uint64_t ms, ml; } cdc_t;   /* files up to repo->whole bytes skip the chunker */
void cdc_init(cdc_t *c, uint32_t avg);
size_t cdc_cut(const cdc_t *c, const uint8_t *p, size_t n);
uint32_t avg_round(uint32_t avg);

/* repository */
void repo_zero(repo_t *r);
int  repo_create(repo_t *r, const char *dir, uint32_t avg, uint32_t whole);       /* new empty repo in memory; makes the directory */
int  repo_open(repo_t *r, const char *dir);                       /* DIR or DIR/.cdcstore; 0 or -1 (says why) */
int  repo_save(repo_t *r);                                        /* sorts entries, atomic */
void repo_free(repo_t *r);
size_t str_add(repo_t *r, const char *s);
#define STR(r, off) ((r)->str + (off))
size_t chunk_insert(repo_t *r, const chunk_t *c);                 /* returns its number */
entry_t *entry_new(repo_t *r);
long entry_find(const repo_t *r, const char *path);               /* binary search; only valid right after load/save */
void chunk_path(const repo_t *r, uint32_t id, char *out, size_t sz);                /* by chunk file number */
int  chunk_write(const repo_t *r, uint32_t id, const uint8_t *data, uint32_t len); /* 0, or -1 after saying why */
int  chunk_read(const repo_t *r, uint32_t row, uint8_t *out);                      /* chunk table row; reads len bytes; 0 or -1 */
int  chunk_delete(const repo_t *r, uint32_t id);
/* Drops the chunk table rows (duplicates in rows[] are fine) and renumbers the rows that stay, in
 * every file's chunk list. Returns how many chunk file numbers were dropped (in *ids, malloc'd, for
 * the caller to delete AFTER the index is saved). Call after repo_save() so refs belong to live files. */
size_t repo_drop_chunks(repo_t *r, const uint32_t *rows, size_t n, uint32_t **ids);

/* paths and patterns */
char *norm_path(const char *p, int *bad);                         /* strips ./ and leading /, collapses //; bad=1 if it has .. */
int  path_under(const char *base, const char *path, const char **rel);
int  sel_match(char **pats, int n, const char *path);             /* n == 0 selects everything */
int  mkdir_p(const char *path, mode_t mode);
void human(uint64_t v, char *out, size_t n);
int  ask_danger(const char *what);                                /* 1 if the user typed DELETE on the terminal */

/* Reads back every selected file from the chunk files: every chunk must have its length and hash to what the
 * index says, offsets and sizes must add up, and the file hash must match. With cmpbase, each file
 * whose original exists at cmpbase/PATH is also compared byte for byte (originals that are gone are
 * skipped). Returns the number of problems (each is reported on stderr); *nfiles gets the files read. */
uint64_t repo_verify(const repo_t *r, char **pats, int npats, const char *cmpbase, uint64_t *nfiles, uint64_t *nbytes);
#endif
