/*
 * cdcstore-restore - take files out of a cdcstore repository
 *
 *   cdcstore-restore [-o OUTDIR] [-f] [-q] REPO [PATTERN...]
 *
 * REPO is the repository folder (or a folder holding .cdcstore, as -X makes). With no PATTERN
 * everything is restored. A PATTERN is a path (a file, or a folder with everything in it) or a
 * wildcard (* ? [..]) matched against the stored path; a wildcard also takes in what is below a
 * matching folder. Quote wildcards so the shell leaves them alone: 'GAMES/DOOM*' '*.EXE'.
 * Files go under OUTDIR (default .) with their stored paths, modes and times.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#include "cdcstore-common.h"

static void usage(void) {
    fprintf(stderr,
        "Usage: cdcstore-restore [-o OUTDIR] [-f] [-q] REPO [PATTERN...]\n"
        "  -o DIR  restore under DIR (default: the current folder; made if missing)\n"
        "  -f      overwrite files that already exist (otherwise they are skipped and reported)\n"
        "  -q      quiet     -h  this help\n"
        "PATTERN: a path (file, or folder with its contents) or a quoted wildcard; none = everything\n");
}

int main(int argc, char **argv) {
    const char *out = ".";
    int force = 0, quiet = 0, opt;
    while ((opt = getopt(argc, argv, "o:fqh")) != -1) {
        switch (opt) {
            case 'o': out = optarg; break;
            case 'f': force = 1; break;
            case 'q': quiet = 1; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }
    if (optind >= argc) { usage(); return 1; }
    repo_t r;
    if (repo_open(&r, argv[optind]) != 0) return 1;
    char **pats = argv + optind + 1;
    int np = argc - optind - 1;
    if (mkdir_p(out, 0755) != 0) { fprintf(stderr, "cdcstore-restore: cannot make %s: %s\n", out, strerror(errno)); return 1; }

    uint32_t maxc = r.avg * 8;
    if (r.whole > maxc) maxc = r.whole;
    uint8_t *tmp = malloc(maxc);
    uint64_t nfiles = 0, nbytes = 0, skipped = 0, errors = 0, matched = 0;
    for (size_t i = 0; i < r.nen; i++) {
        const entry_t *e = &r.en[i];
        const char *path = STR(&r, e->path_off);
        if (!sel_match(pats, np, path)) continue;
        matched++;
        int bad = 0;
        char *n = norm_path(path, &bad);
        if (bad || strcmp(n, path) != 0 || !*n) { fprintf(stderr, "cdcstore-restore: refusing the unsafe stored path '%s'\n", path); free(n); errors++; continue; }
        free(n);
        char dst[PATH_MAX + 64];
        snprintf(dst, sizeof(dst), "%s/%s", out, path);
        if (e->type == T_DIR) { mkdir_p(dst, 0755); continue; }
        char *sl = strrchr(dst, '/');
        if (sl) { *sl = 0; mkdir_p(dst, 0755); *sl = '/'; }
        struct stat st;
        if (lstat(dst, &st) == 0) {
            if (!force) { if (!quiet) fprintf(stderr, "cdcstore-restore: %s exists, skipped (-f overwrites)\n", dst); skipped++; continue; }
            if (S_ISDIR(st.st_mode)) { fprintf(stderr, "cdcstore-restore: %s is a folder, skipped\n", dst); errors++; continue; }
            unlink(dst);
        }
        if (e->type == T_LINK) {
            if (symlink(STR(&r, e->aux_off), dst) != 0) { fprintf(stderr, "cdcstore-restore: cannot make the link %s: %s\n", dst, strerror(errno)); errors++; }
            else nfiles++;
            continue;
        }
        int fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { fprintf(stderr, "cdcstore-restore: cannot create %s: %s\n", dst, strerror(errno)); errors++; continue; }
        h128_t fh = fh_start();
        uint64_t total = 0;
        int ok = 1;
        for (uint32_t k = 0; k < e->nref && ok; k++) {
            const ref_t *rf = &r.refs[e->ref_start + k];
            if (rf->chunk >= r.nch || r.ch[rf->chunk].len > maxc || chunk_read(&r, rf->chunk, tmp) != 0) { ok = 0; break; }
            const chunk_t *c = &r.ch[rf->chunk];
            h128_t h = hash128(tmp, c->len);
            if (h.a != c->h0 || h.b != c->h1) { fprintf(stderr, "cdcstore-restore: %s: a chunk fails its hash (damaged chunk file)\n", path); ok = 0; break; }
            fh = fh_fold(fh, h, c->len);
            size_t off = 0;
            while (off < c->len) {
                ssize_t wn = write(fd, tmp + off, c->len - off);
                if (wn < 0) { if (errno == EINTR) continue; fprintf(stderr, "cdcstore-restore: write error on %s: %s\n", dst, strerror(errno)); ok = 0; break; }
                off += (size_t)wn;
            }
            total += c->len;
        }
        if (ok && (total != e->size || fh.a != e->fh0 || fh.b != e->fh1)) { fprintf(stderr, "cdcstore-restore: %s: size or hash does not match the index\n", path); ok = 0; }
        if (ok) fchmod(fd, e->mode & 07777);
        close(fd);
        if (!ok) { unlink(dst); errors++; continue; }
        struct timespec ts[2] = { { e->mtime, 0 }, { e->mtime, 0 } };
        utimensat(AT_FDCWD, dst, ts, 0);
        nfiles++; nbytes += total;
    }
    for (size_t i = r.nen; i-- > 0;) {        /* folder modes and times last, innermost first */
        const entry_t *e = &r.en[i];
        const char *path = STR(&r, e->path_off);
        if (e->type != T_DIR || !sel_match(pats, np, path)) continue;
        char dst[PATH_MAX + 64];
        snprintf(dst, sizeof(dst), "%s/%s", out, path);
        chmod(dst, (e->mode & 07777) | 0700);
        struct timespec ts[2] = { { e->mtime, 0 }, { e->mtime, 0 } };
        utimensat(AT_FDCWD, dst, ts, 0);
    }
    if (np && !matched) fprintf(stderr, "cdcstore-restore: nothing in the repository matches\n");
    if (!quiet) {
        char hb[32];
        human(nbytes, hb, sizeof(hb));
        fprintf(stderr, "cdcstore-restore: %llu files (%s) restored to %s%s%s\n", (unsigned long long)nfiles, hb, out,
                skipped ? ", some skipped (already there)" : "", errors ? ", WITH ERRORS" : "");
    }
    free(tmp);
    repo_free(&r);
    return errors ? 2 : (np && !matched ? 1 : 0);
}
