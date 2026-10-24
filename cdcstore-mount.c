/*
 * cdcstore-mount - mount a cdcstore repository as a read-only folder (FUSE).
 *
 *   cdcstore-mount [-f] [-d] [-o OPT,...] REPO MOUNTPOINT
 *
 * REPO is a repository folder (or a folder that holds .cdcstore). The files appear with their
 * stored paths, modes, times and symlinks; reading a file reads the chunk files it is made of,
 * only the ones the read touches. All chunk files must be present (it is the same repository
 * that cdcstore-restore reads, for example extracted from the archive you made with
 * cdcstore-list -k). Nothing is cached or decoded: the repository is plain files.
 * Unmount with: fusermount3 -u MOUNTPOINT
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include "cdcstore-common.h"

#define NAME "cdcstore-mount"

static repo_t g_repo;

typedef struct {                  /* one open file; the last chunk file stays open for sequential reads */
    uint32_t ent;
    pthread_mutex_t lock;
    int cfd;
    uint32_t crow;
} handle_t;

static long lookup(const char *path) {
    while (*path == '/') path++;
    if (!*path) return -2;                                        /* the root folder */
    return entry_find(&g_repo, path);
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void fill_stat(const entry_t *e, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_mtime = (time_t)e->mtime;
    st->st_atime = st->st_ctime = st->st_mtime;
    mode_t perm = (mode_t)(e->mode & 07777) & ~(mode_t)0222;      /* read-only */
    if (e->type == T_DIR) { st->st_mode = S_IFDIR | perm | 0500; st->st_nlink = 2; }
    else if (e->type == T_LINK) { st->st_mode = S_IFLNK | 0777; st->st_nlink = 1; st->st_size = (off_t)strlen(STR(&g_repo, e->aux_off)); }
    else { st->st_mode = S_IFREG | perm; st->st_nlink = 1; st->st_size = (off_t)e->size; st->st_blocks = (blkcnt_t)((e->size + 511) / 512); }
}

static int cm_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    long i = lookup(path);
    if (i == -2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0555; st->st_nlink = 2; st->st_uid = getuid(); st->st_gid = getgid();
        st->st_mtime = st->st_atime = st->st_ctime = time(NULL);
        return 0;
    }
    if (i < 0) return -ENOENT;
    fill_stat(&g_repo.en[i], st);
    return 0;
}

static int cm_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off, struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    long d = lookup(path);
    size_t i, stop;
    if (d == -2) { i = 0; stop = g_repo.nen; }
    else if (d < 0) return -ENOENT;
    else if (g_repo.en[d].type != T_DIR) return -ENOTDIR;
    else { i = (size_t)d + 1; stop = g_repo.en[d].end; }
    fill(buf, ".", NULL, 0, 0);
    fill(buf, "..", NULL, 0, 0);
    while (i < stop) {
        const entry_t *e = &g_repo.en[i];
        struct stat st;
        fill_stat(e, &st);
        if (fill(buf, base_name(STR(&g_repo, e->path_off)), &st, 0, 0)) break;
        i = e->end;
    }
    return 0;
}

static int cm_readlink(const char *path, char *buf, size_t size) {
    long i = lookup(path);
    if (i < 0) return -ENOENT;
    if (g_repo.en[i].type != T_LINK) return -EINVAL;
    snprintf(buf, size, "%s", STR(&g_repo, g_repo.en[i].aux_off));
    return 0;
}

static int cm_open(const char *path, struct fuse_file_info *fi) {
    long i = lookup(path);
    if (i < 0) return -ENOENT;
    if (g_repo.en[i].type == T_DIR) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    handle_t *h = calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;
    h->ent = (uint32_t)i; h->cfd = -1; h->crow = NO_ENTRY;
    pthread_mutex_init(&h->lock, NULL);
    fi->fh = (uint64_t)(uintptr_t)h;
    fi->keep_cache = 1;
    return 0;
}

static int cm_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    if (h) { if (h->cfd >= 0) close(h->cfd); pthread_mutex_destroy(&h->lock); free(h); }
    return 0;
}

/* bytes [from, from+n) of a chunk; the handle's open chunk file is reused if it is the same chunk */
static int chunk_pread(handle_t *h, uint32_t row, uint8_t *out, size_t n, uint64_t from) {
    if (row >= g_repo.nch) return -EIO;
    if (h->crow != row) {
        if (h->cfd >= 0) close(h->cfd);
        char p[PATH_MAX + 64];
        chunk_path(&g_repo, g_repo.ch[row].id, p, sizeof(p));
        h->cfd = open(p, O_RDONLY);
        h->crow = row;
        if (h->cfd < 0) { int e = errno; fprintf(stderr, NAME ": cannot open %s: %s\n", p, strerror(e)); return -e; }
    }
    size_t got = 0;
    while (got < n) {
        ssize_t k = pread(h->cfd, out + got, n - got, (off_t)(from + got));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -EIO;
        got += (size_t)k;
    }
    return 0;
}

static int cm_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)path;
    handle_t *h = (handle_t *)(uintptr_t)fi->fh;
    const entry_t *e = &g_repo.en[h->ent];
    if (off < 0) return -EINVAL;
    uint64_t pos = (uint64_t)off;
    if (pos >= e->size) return 0;
    if (size > e->size - pos) size = (size_t)(e->size - pos);
    const ref_t *rf = g_repo.refs + e->ref_start;
    /* the last chunk that begins at or before pos */
    size_t lo = 0, hi = e->nref;
    while (hi - lo > 1) { size_t mid = (lo + hi) / 2; if (rf[mid].off <= pos) lo = mid; else hi = mid; }
    size_t done = 0;
    int rc = 0;
    pthread_mutex_lock(&h->lock);
    for (size_t k = lo; k < e->nref && done < size; k++) {
        uint32_t row = rf[k].chunk;
        if (row >= g_repo.nch) { rc = -EIO; break; }
        uint64_t cstart = rf[k].off, clen = g_repo.ch[row].len;
        uint64_t from = pos + done - cstart;
        if (from >= clen) { rc = -EIO; break; }
        size_t n = (size_t)(clen - from);
        if (n > size - done) n = size - done;
        rc = chunk_pread(h, row, (uint8_t *)buf + done, n, from);
        if (rc) break;
        done += n;
    }
    pthread_mutex_unlock(&h->lock);
    return rc ? rc : (int)done;
}

static int cm_statfs(const char *path, struct statvfs *sv) {
    (void)path;
    memset(sv, 0, sizeof(*sv));
    sv->f_bsize = sv->f_frsize = 4096;
    sv->f_blocks = (fsblkcnt_t)((g_repo.nen ? 0 : 0) + 0);
    sv->f_namemax = 255;
    return 0;
}

static const struct fuse_operations cm_ops = {
    .getattr  = cm_getattr,
    .readdir  = cm_readdir,
    .readlink = cm_readlink,
    .open     = cm_open,
    .release  = cm_release,
    .read     = cm_read,
    .statfs   = cm_statfs,
};

static void usage(FILE *fp) {
    fprintf(fp,
        "usage: " NAME " [options] REPO MOUNTPOINT\n\n"
        "Mounts a cdcstore repository read-only: the stored files with their paths, modes, times\n"
        "and links. A read fetches only the chunk files it needs. All chunk files must be present.\n\n"
        "Options are passed to libfuse:\n"
        "  -f          stay in the foreground (errors are printed to the terminal)\n"
        "  -d          foreground plus libfuse debug output\n"
        "  -o OPT,...  mount options, e.g. -o allow_other\n"
        "Unmount with: fusermount3 -u MOUNTPOINT\n");
}

int main(int argc, char *argv[]) {
    const char *repo = NULL, *mnt = NULL;
    char **fargv = calloc((size_t)argc + 4, sizeof(char *));
    int fargc = 0;
    if (!fargv) return 1;
    fargv[fargc++] = argv[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "-o")) {
            if (i + 1 >= argc) { usage(stderr); return 1; }
            fargv[fargc++] = argv[i];
            fargv[fargc++] = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            fargv[fargc++] = argv[i];
        } else if (!repo) {
            repo = a;
        } else if (!mnt) {
            mnt = a;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (!repo || !mnt) { usage(stderr); return 1; }
    repo_zero(&g_repo);
    if (repo_open(&g_repo, repo) != 0) return 1;
    char *abs = realpath(g_repo.dir, NULL);          /* libfuse changes to / when it goes to the background */
    if (!abs) { fprintf(stderr, NAME ": %s: %s\n", g_repo.dir, strerror(errno)); return 1; }
    free(g_repo.dir);
    g_repo.dir = abs;
    /* a missing chunk shows up as an error on read; say now if the chunk folder is not there at all */
    char cd[PATH_MAX + 32];
    snprintf(cd, sizeof(cd), "%s/%s", g_repo.dir, CHUNK_DIR);
    struct stat st;
    if (g_repo.nch && (stat(cd, &st) != 0 || !S_ISDIR(st.st_mode))) {
        fprintf(stderr, NAME ": %s has an index but no %s folder (extract all the chunk files next to it)\n", g_repo.dir, CHUNK_DIR);
        return 1;
    }
    fprintf(stderr, NAME ": %zu entries, %zu chunks\n", g_repo.nen, g_repo.nch);
    fargv[fargc++] = (char *)mnt;
    fargv[fargc++] = (char *)"-o";
    fargv[fargc++] = (char *)"ro";
    return fuse_main(fargc, fargv, &cm_ops, NULL);
}
