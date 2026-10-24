/*
 * cdcstore-add - store files in a cdcstore repository (cut into plain chunk files)
 *
 *   cdcstore-add -d DEST [options] SOURCE...      originals stay where they are
 *   cdcstore-add -X      [options] DIR            IN PLACE: DIR becomes the repository (DIR/.cdcstore);
 *                                                 the originals are DELETED once stored and verified
 *
 * A SOURCE is a directory (scanned, recursively), a file, or - (a list of paths on stdin, in the
 * order they are to be stored: from sortof-natural, sortof-minhash, find, ...). The order is kept;
 * nothing is sorted here, so any sorter fits. Files up to -s bytes are one chunk each; larger
 * files are cut into content-defined chunks of about -a bytes (a chunk never spans two files).
 * Every chunk is written as its own file (chunks/bNNNNN/cNNNNNNNNN, numbered in the order they
 * are made). Nothing is deduplicated, compressed or packed here: that is for the sorter and the
 * archiver, which can take the chunk list from cdcstore-list -k. Adding to an existing repository keeps ITS -a and -s.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#include "cdcstore-common.h"

typedef struct { char *path; char *rel; struct stat st; int type; char *target; int skip; } item_t;

static item_t *items; static size_t nitems, capitems;
static int quiet;
static uint32_t *rel_chunks; static size_t nrel, caprel;       /* chunks of replaced files: deleted after the new index is saved */

static void item_push(const char *path, const struct stat *st, int type, const char *target) {
    if (nitems == capitems) {
        capitems = capitems ? capitems * 2 : 1024;
        items = realloc(items, capitems * sizeof(item_t));
        if (!items) { fprintf(stderr, "cdcstore-add: out of memory\n"); exit(1); }
    }
    item_t *it = &items[nitems++];
    memset(it, 0, sizeof(*it));
    it->path = strdup(path); it->st = *st; it->type = type;
    it->target = target ? strdup(target) : NULL;
}

static void release_old(repo_t *r, long old) {
    if (old < 0) return;
    entry_t *e = &r->en[old];
    if (e->type == T_FILE)
        for (uint32_t k = 0; k < e->nref; k++) {
            if (nrel == caprel) { caprel = caprel ? caprel * 2 : 1024; rel_chunks = realloc(rel_chunks, caprel * sizeof(uint32_t)); }
            rel_chunks[nrel++] = r->refs[e->ref_start + k].chunk;
        }
    e->type = T_DEL;
}

static char *skipdir_real;      /* the repository folder: never scanned into itself */

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void classify(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) { fprintf(stderr, "cdcstore-add: cannot stat %s: %s\n", path, strerror(errno)); return; }
    if (S_ISREG(st.st_mode)) { item_push(path, &st, T_FILE, NULL); return; }
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX + 1];
        ssize_t n = readlink(path, t, PATH_MAX);
        if (n < 0) { fprintf(stderr, "cdcstore-add: cannot read the link %s: %s\n", path, strerror(errno)); return; }
        t[n] = 0;
        item_push(path, &st, T_LINK, t);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        char *rp = realpath(path, NULL);
        if (rp && skipdir_real && !strcmp(rp, skipdir_real)) { free(rp); return; }
        free(rp);
        item_push(path, &st, T_DIR, NULL);
        DIR *d = opendir(path);
        if (!d) { fprintf(stderr, "cdcstore-add: cannot read %s: %s\n", path, strerror(errno)); return; }
        char **names = NULL; size_t nn = 0, cn = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (nn == cn) { cn = cn ? cn * 2 : 64; names = realloc(names, cn * sizeof(char *)); }
            names[nn++] = strdup(de->d_name);
        }
        closedir(d);
        qsort(names, nn, sizeof(char *), cmp_str);
        for (size_t i = 0; i < nn; i++) {
            char sub[PATH_MAX + 512];
            snprintf(sub, sizeof(sub), "%s/%s", path, names[i]);
            classify(sub);
            free(names[i]);
        }
        free(names);
        return;
    }
    fprintf(stderr, "cdcstore-add: skipping %s (not a file, folder or link)\n", path);
}

static int cmp_item_rel(const void *a, const void *b) {
    const item_t *x = *(item_t *const *)a, *y = *(item_t *const *)b;
    int c = strcmp(x->rel, y->rel);
    if (c) return c;
    return x > y ? 1 : -1;                         /* same name: the earlier one first */
}

static void usage(void) {
    fprintf(stderr,
        "Usage: cdcstore-add -d DEST [options] SOURCE...     store into the repository DEST (made if new)\n"
        "       cdcstore-add -X [options] DIR                IN PLACE: DIR/.cdcstore holds the data, the\n"
        "                                                    originals in DIR are DELETED after a full check\n"
        "SOURCE: a folder (scanned), a file, or - (paths on stdin, kept in the order given)\n"
        "Options:\n"
        "  -a N   average chunk size for files above -s (default %u; rounded to a power of two, 1 KiB..4 MiB)\n"
        "  -s N   files up to N bytes are stored whole, one chunk (default %u)\n"
        "  -C DIR store paths relative to DIR (like tar -C); a path not under DIR is an error\n"
        "  -y     with -X: do not ask before deleting (the DANGER notice is still printed)\n"
        "  -S     skip the final sync of the disk (faster; after a power cut verify can find missing chunk files)\n"
        "  -q     quiet     -h  this help\n"
        "Paths are stored without a leading / or ./ and may not contain ..\n"
        "-a and -s only matter for a new repository; an existing one keeps its own.\n", DEF_AVG, DEF_WHOLE);
}

int main(int argc, char **argv) {
    const char *dest = NULL, *cbase = NULL;
    int inplace = 0, yes = 0, nosync = 0, opt;
    uint32_t avg = 0, whole = 0;
    while ((opt = getopt(argc, argv, "d:Xa:s:C:ySqh")) != -1) {
        switch (opt) {
            case 'd': dest = optarg; break;
            case 'X': inplace = 1; break;
            case 'a': avg = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 's': whole = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'C': cbase = optarg; break;
            case 'y': yes = 1; break;
            case 'S': nosync = 1; break;
            case 'q': quiet = 1; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }
    if (!!dest == !!inplace) { fprintf(stderr, "cdcstore-add: give exactly one of -d DEST or -X (in place)\n"); usage(); return 1; }
    if (optind >= argc) { usage(); return 1; }
    int nsrc = argc - optind;
    char **src = argv + optind;
    char repodir[PATH_MAX], *base = NULL;
    if (inplace) {
        struct stat st;
        if (nsrc != 1 || cbase) { fprintf(stderr, "cdcstore-add: -X takes exactly one folder and no -C\n"); return 1; }
        if (!strcmp(src[0], "-") || stat(src[0], &st) != 0 || !S_ISDIR(st.st_mode)) { fprintf(stderr, "cdcstore-add: %s is not a folder\n", src[0]); return 1; }
        base = norm_path(src[0], NULL);
        char *sdir = strdup(src[0]);
        size_t l = strlen(sdir);
        while (l > 1 && sdir[l - 1] == '/') sdir[--l] = 0;
        snprintf(repodir, sizeof(repodir), "%s/%s", sdir, INPLACE_DIR);
        free(sdir);
    } else {
        snprintf(repodir, sizeof(repodir), "%s", dest);
        if (cbase) base = norm_path(cbase, NULL);
    }
    if (avg && (avg < 1024 || avg > (1u << 22))) { fprintf(stderr, "cdcstore-add: -a must be between 1024 and 4194304\n"); return 1; }
    if (whole > (16u << 20)) { fprintf(stderr, "cdcstore-add: -s may not be above 16777216\n"); return 1; }

    /* the repository */
    repo_t r;
    char ip[PATH_MAX + 64];
    snprintf(ip, sizeof(ip), "%s/%s", repodir, IDX_NAME);
    int existing = access(ip, F_OK) == 0;
    if (existing) {
        if (repo_open(&r, repodir) != 0) return 1;
        if ((avg && avg_round(avg) != r.avg) || (whole && whole != r.whole))
            fprintf(stderr, "cdcstore-add: note: %s already exists with -a %u -s %u; keeping those\n", repodir, r.avg, r.whole);
    } else {
        if (repo_create(&r, repodir, avg ? avg : DEF_AVG, whole ? whole : DEF_WHOLE) != 0) return 1;
    }
    mkdir_p(repodir, 0755);
    skipdir_real = realpath(repodir, NULL);

    /* the files, in order */
    for (int a = 0; a < nsrc; a++) {
        if (!strcmp(src[a], "-")) {
            char line[PATH_MAX];
            while (fgets(line, sizeof(line), stdin)) {
                size_t l = strlen(line);
                while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
                if (l) classify(line);
            }
        } else classify(src[a]);
    }
    for (size_t i = 0; i < nitems; i++) {
        int bad = 0;
        char *n = norm_path(items[i].path, &bad);
        const char *rel = n;
        if (base && !path_under(base, n, &rel)) { fprintf(stderr, "cdcstore-add: %s is not under %s\n", items[i].path, cbase ? cbase : src[0]); return 1; }
        if (bad) { fprintf(stderr, "cdcstore-add: %s has a .. in it; use -C to store paths relative to a folder\n", items[i].path); return 1; }
        items[i].rel = strdup(rel);
        free(n);
        if (!items[i].rel[0]) items[i].skip = 1;          /* the base folder itself */
    }
    {   /* the same stored path twice: the first wins */
        item_t **ix = malloc((nitems + 1) * sizeof(item_t *));
        for (size_t i = 0; i < nitems; i++) ix[i] = &items[i];
        qsort(ix, nitems, sizeof(item_t *), cmp_item_rel);
        for (size_t i = 1; i < nitems; i++)
            if (!strcmp(ix[i]->rel, ix[i - 1]->rel) && ix[i]->rel[0]) {
                if (!quiet) fprintf(stderr, "cdcstore-add: note: %s is listed twice; only the first is stored\n", ix[i]->rel);
                ix[i]->skip = 1;
            }
        free(ix);
    }
    uint64_t total_in = 0, nfiles = 0;
    for (size_t i = 0; i < nitems; i++) if (!items[i].skip && items[i].type == T_FILE) { nfiles++; total_in += (uint64_t)items[i].st.st_size; }
    if (nitems == 0) { fprintf(stderr, "cdcstore-add: nothing to store\n"); return 1; }

    if (inplace) {
        char hb[32], msg[PATH_MAX + 512];
        human(total_in, hb, sizeof(hb));
        snprintf(msg, sizeof(msg), "IN PLACE: %llu files (%s) in %s will be stored in %s,\nchecked byte for byte, and then THE ORIGINALS WILL BE DELETED.\nOnly the repository is left. Back up first if you cannot afford a mistake.",
                 (unsigned long long)nfiles, hb, src[0], repodir);
        if (yes) fprintf(stderr, "\n*** DANGER ***\n%s\n(-y given: not asking)\n", msg);
        else if (!ask_danger(msg)) { fprintf(stderr, "cdcstore-add: stopped, nothing was changed\n"); return 1; }
    }

    cdc_t cdc;
    cdc_init(&cdc, r.avg);
    uint32_t maxc = cdc.max > r.whole ? cdc.max : r.whole;
    uint8_t *buf = malloc((size_t)(1u << 20) + maxc + 16);
    if (!buf) { fprintf(stderr, "cdcstore-add: out of memory\n"); return 1; }

    uint64_t added = 0, unchanged = 0, newbytes = 0, newchunks = 0, failed = 0;
    time_t t0 = time(NULL);
    for (size_t i = 0; i < nitems; i++) {
        item_t *it = &items[i];
        if (it->skip) continue;
        long old = entry_find(&r, it->rel);
        if (it->type == T_DIR) {
            if (old >= 0 && r.en[old].type == T_DIR) { r.en[old].mode = it->st.st_mode; r.en[old].mtime = it->st.st_mtime; continue; }
            release_old(&r, old);
            size_t po = str_add(&r, it->rel);
            entry_t *e = entry_new(&r);
            e->path_off = po; e->type = T_DIR; e->mode = it->st.st_mode; e->mtime = it->st.st_mtime;
            continue;
        }
        if (it->type == T_LINK) {
            if (old >= 0 && r.en[old].type == T_LINK && !strcmp(STR(&r, r.en[old].aux_off), it->target)) { unchanged++; continue; }
            release_old(&r, old);
            size_t po = str_add(&r, it->rel), ao = str_add(&r, it->target);
            entry_t *e = entry_new(&r);
            e->path_off = po; e->aux_off = ao; e->type = T_LINK; e->mode = it->st.st_mode; e->mtime = it->st.st_mtime;
            added++;
            continue;
        }
        if (old >= 0 && r.en[old].type == T_FILE && r.en[old].size == (uint64_t)it->st.st_size && r.en[old].mtime == it->st.st_mtime) {
            unchanged++;
            continue;
        }
        int fd = open(it->path, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cdcstore-add: cannot read %s: %s\n", it->path, strerror(errno)); failed++; it->skip = 1; continue; }
        size_t ref0 = r.nrefs, nch0 = r.nch;
        int rerr = 0;
        h128_t fh = fh_start();
        uint64_t total = 0;
        int ok = 1;
#define EMIT(P, L) do { \
            h128_t h_ = hash128((P), (L)); \
            long ci_; \
            { \
                chunk_t c_; c_.h0 = h_.a; c_.h1 = h_.b; c_.len = (uint32_t)(L); c_.id = r.next_id; \
                if (chunk_write(&r, c_.id, (P), (uint32_t)(L)) != 0) { ok = 0; break; } \
                r.next_id++; \
                ci_ = (long)chunk_insert(&r, &c_); newbytes += (L); newchunks++; \
            } \
            if (r.nrefs == r.capref) { r.capref = r.capref ? r.capref * 2 : 1024; r.refs = realloc(r.refs, r.capref * sizeof(ref_t)); if (!r.refs) { fprintf(stderr, "cdcstore-add: out of memory\n"); exit(1); } } \
            r.refs[r.nrefs].off = total; r.refs[r.nrefs].chunk = (uint32_t)ci_; r.nrefs++; \
            fh = fh_fold(fh, h_, (uint32_t)(L)); total += (L); \
        } while (0)
        if ((uint64_t)it->st.st_size <= r.whole) {
            size_t got = 0;
            while (got < r.whole) {
                ssize_t n = read(fd, buf + got, r.whole - got);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) rerr = errno;
                if (n <= 0) break;
                got += (size_t)n;
            }
            if (got && !rerr) EMIT(buf, got);
        } else {
            size_t len = 0, pos = 0;
            int eof = 0;
            const size_t cap = (size_t)(1u << 20) + maxc;
            while (ok) {
                while (!eof && len < cap) {
                    ssize_t n = read(fd, buf + len, cap - len);
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0) rerr = errno;
                    if (n <= 0) { eof = 1; break; }
                    len += (size_t)n;
                }
                if (rerr) break;
                if (pos >= len) {                      /* everything in the buffer is used: read more, unless that was the end */
                    if (eof) break;
                    pos = len = 0;
                    continue;
                }
                if (!eof && len - pos < maxc) { memmove(buf, buf + pos, len - pos); len -= pos; pos = 0; continue; }
                size_t cut = cdc_cut(&cdc, buf + pos, len - pos);
                EMIT(buf + pos, cut);
                if (!ok) break;
                pos += cut;
            }
        }
        close(fd);
        if (!ok) { fprintf(stderr, "cdcstore-add: stopped while storing %s; the index was not changed (chunk files written so far are overwritten by the next run)\n", it->path); return 1; }
        struct stat st2;
        int now_ok = stat(it->path, &st2) == 0;
        int bad_read = rerr || total != (uint64_t)it->st.st_size;
        if (bad_read) {      /* never record a file that was not read completely: undo its chunks */
            if (rerr) fprintf(stderr, "cdcstore-add: %s: read error after %llu bytes: %s; not stored\n", it->path, (unsigned long long)total, strerror(rerr));
            else if (now_ok && (st2.st_size != it->st.st_size || st2.st_mtime != it->st.st_mtime))
                fprintf(stderr, "cdcstore-add: %s changed while it was read (%llu bytes seen, %llu expected); not stored, run again\n", it->path, (unsigned long long)total, (unsigned long long)it->st.st_size);
            else
                fprintf(stderr, "cdcstore-add: %s: only %llu of %llu bytes could be read (the file did not change; a disk or mount problem?); not stored\n", it->path, (unsigned long long)total, (unsigned long long)it->st.st_size);
            for (size_t c = nch0; c < r.nch; c++) chunk_delete(&r, r.ch[c].id);
            if (r.nch > nch0) r.next_id = r.ch[nch0].id;
            r.nch = nch0; r.nrefs = ref0;
            failed++; it->skip = 1;
            continue;
        }
        release_old(&r, old);
        size_t po = str_add(&r, it->rel);
        entry_t *e = entry_new(&r);
        e->path_off = po; e->type = T_FILE; e->mode = it->st.st_mode; e->mtime = it->st.st_mtime;
        e->size = total; e->ref_start = ref0; e->nref = (uint32_t)(r.nrefs - ref0); e->fh0 = fh.a; e->fh1 = fh.b;
        added++;
        if (!quiet && (added & 255) == 0) { fprintf(stderr, "\r%llu files...", (unsigned long long)added); fflush(stderr); }
    }
    {   /* the chunk files must be on disk before the index that names them */
        int dfd = nosync ? -1 : open(repodir, O_RDONLY);
        if (dfd >= 0) { syncfs(dfd); close(dfd); }
    }
    if (repo_save(&r) != 0) return 1;
    if (nrel) {      /* chunks of files that were replaced: nobody uses them now */
        uint32_t *ids;
        size_t nid = repo_drop_chunks(&r, rel_chunks, nrel, &ids);
        if (repo_save(&r) != 0) return 1;
        for (size_t k = 0; k < nid; k++) chunk_delete(&r, ids[k]);
        free(ids);
    }
    if (!quiet) {
        char b[32];
        human(newbytes, b, sizeof(b));
        fprintf(stderr, "\rcdcstore-add: %llu stored, %llu unchanged%s in %lds. %s written as %llu chunk files.\n",
                (unsigned long long)added, (unsigned long long)unchanged, failed ? " (some could not be read)" : "", (long)(time(NULL) - t0),
                b, (unsigned long long)newchunks);
    }
    int rc = failed ? 2 : 0;

    if (inplace) {
        if (failed) { fprintf(stderr, "cdcstore-add: some files could not be read, so nothing is deleted\n"); return 2; }
        if (!quiet) fprintf(stderr, "cdcstore-add: checking everything against the originals before deleting...\n");
        uint64_t nf, nb;
        uint64_t bad = repo_verify(&r, NULL, 0, base, &nf, &nb);
        if (bad) { fprintf(stderr, "cdcstore-add: %llu problem(s): NOTHING WAS DELETED\n", (unsigned long long)bad); return 2; }
        uint64_t del = 0;
        for (size_t i = 0; i < nitems; i++)
            if (!items[i].skip && (items[i].type == T_FILE || items[i].type == T_LINK) && unlink(items[i].path) == 0) del++;
        for (size_t i = nitems; i-- > 0;)
            if (!items[i].skip && items[i].type == T_DIR) rmdir(items[i].path);      /* only if empty */
        if (!quiet) fprintf(stderr, "cdcstore-add: verified %llu files; deleted %llu originals. The data now lives in %s\n", (unsigned long long)nf, (unsigned long long)del, repodir);
    }
    (void)total_in;
    repo_free(&r);
    return rc;
}
