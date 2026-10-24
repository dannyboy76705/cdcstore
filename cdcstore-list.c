/*
 * cdcstore-list - list what a cdcstore repository holds
 *
 *   cdcstore-list [-l] [-s] [-k] REPO [PATTERN...]
 *
 * Plain: one stored path per line (folders end in /). -l adds type, mode, size and time.
 * -s prints the repository's numbers instead: files, sizes, unique chunks, saving.
 * -k prints the chunk files (paths, in chunk-number order, i.e. the order the data was added) instead,
 * after the path of cdcstore.idx: the list to give a sorter (sortof-*, which keeps cdcstore.idx first without
 * sorting or reading it) and then an archiver. With PATTERNs only the chunks those files
 * use are printed, each once, in the order they are first used.
 * PATTERN: a path (file, or folder with its contents) or a quoted wildcard; none = everything.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#include "cdcstore-common.h"

static void usage(void) {
    fprintf(stderr,
        "Usage: cdcstore-list [-l] [-s] [-k] REPO [PATTERN...]\n"
        "  -l  long: type, mode, size, date, path\n"
        "  -s  statistics of the repository (and of the selection)\n"
        "  -k  cdcstore.idx, then the chunk files, one path per line (for a sorter and an archiver)\n"
        "  -h  this help\n");
}

int main(int argc, char **argv) {
    int lng = 0, stats = 0, chunks = 0, opt;
    while ((opt = getopt(argc, argv, "lskh")) != -1) {
        switch (opt) {
            case 'l': lng = 1; break;
            case 's': stats = 1; break;
            case 'k': chunks = 1; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }
    if (optind >= argc) { usage(); return 1; }
    repo_t r;
    if (repo_open(&r, argv[optind]) != 0) return 1;
    char **pats = argv + optind + 1;
    int np = argc - optind - 1;
    if (chunks) {
        char p[PATH_MAX + 64];
        printf("%s/%s\n", r.dir, IDX_NAME);                    /* the index goes in the archive too, first */
        if (np == 0) {
            for (size_t i = 0; i < r.nch; i++) {
                chunk_path(&r, r.ch[i].id, p, sizeof(p));
                puts(p);
            }
        } else {
            uint8_t *seen = calloc(r.nch + 1, 1);
            for (size_t i = 0; i < r.nen; i++) {
                const entry_t *e = &r.en[i];
                if (e->type != T_FILE || !sel_match(pats, np, STR(&r, e->path_off))) continue;
                for (uint32_t k = 0; k < e->nref; k++) {
                    uint32_t c = r.refs[e->ref_start + k].chunk;
                    if (c < r.nch && !seen[c]) { seen[c] = 1; chunk_path(&r, r.ch[c].id, p, sizeof(p)); puts(p); }
                }
            }
            free(seen);
        }
        repo_free(&r);
        return 0;
    }
    uint64_t nfile = 0, ndir = 0, nlink = 0, logical = 0, matched = 0;
    for (size_t i = 0; i < r.nen; i++) {
        const entry_t *e = &r.en[i];
        const char *path = STR(&r, e->path_off);
        if (!sel_match(pats, np, path)) continue;
        matched++;
        if (e->type == T_FILE) { nfile++; logical += e->size; }
        else if (e->type == T_DIR) ndir++;
        else nlink++;
        if (stats) continue;
        if (!lng) {
            printf("%s%s\n", path, e->type == T_DIR ? "/" : "");
        } else {
            char tm[32];
            time_t t = (time_t)e->mtime;
            struct tm g;
            gmtime_r(&t, &g);
            strftime(tm, sizeof(tm), "%Y-%m-%d %H:%M", &g);
            printf("%c %04o %12llu %s %s%s%s%s\n", e->type == T_DIR ? 'd' : e->type == T_LINK ? 'l' : '-', e->mode & 07777,
                   (unsigned long long)(e->type == T_FILE ? e->size : 0), tm, path, e->type == T_DIR ? "/" : "",
                   e->type == T_LINK ? " -> " : "", e->type == T_LINK ? STR(&r, e->aux_off) : "");
        }
    }
    if (stats) {
        uint32_t *refc = calloc(r.nch + 1, sizeof(uint32_t));
        for (size_t i = 0; i < r.nrefs; i++) if (r.refs[i].chunk < r.nch) refc[r.refs[i].chunk]++;
        uint64_t chbytes = 0, nlive = 0, uniq_sel = 0, orphan = 0;
        for (size_t i = 0; i < r.nch; i++) {
            chbytes += r.ch[i].len; nlive++;
            if (!refc[i]) orphan++;
        }
        if (np) {   /* unique chunk bytes used by the selection */
            uint8_t *seen = calloc(r.nch + 1, 1);
            for (size_t i = 0; i < r.nen; i++) {
                const entry_t *e = &r.en[i];
                if (e->type != T_FILE || !sel_match(pats, np, STR(&r, e->path_off))) continue;
                for (uint32_t k = 0; k < e->nref; k++) {
                    uint32_t c = r.refs[e->ref_start + k].chunk;
                    if (c < r.nch && !seen[c]) { seen[c] = 1; uniq_sel += r.ch[c].len; }
                }
            }
            free(seen);
        }
        char a[32], b[32];
        printf("repository:        %s\n", r.dir);
        printf("chunk size:        about %u bytes; files up to %u bytes are stored whole\n", r.avg, r.whole);
        printf("selected:          %llu files, %llu folders, %llu links\n", (unsigned long long)nfile, (unsigned long long)ndir, (unsigned long long)nlink);
        human(logical, a, sizeof(a));
        printf("file data:         %s\n", a);
        if (np) { human(uniq_sel, b, sizeof(b)); printf("unique in selection: %s\n", b); }
        human(chbytes, b, sizeof(b));
        printf("whole repository:  %llu chunk files, %s of unique data (before the archiver compresses it)\n", (unsigned long long)nlive, b);
        if (orphan) printf("unused:            %llu chunk files nobody refers to\n", (unsigned long long)orphan);
        free(refc);
    } else if (np && !matched) {
        fprintf(stderr, "cdcstore-list: nothing in the repository matches\n");
        repo_free(&r);
        return 1;
    }
    repo_free(&r);
    return 0;
}
