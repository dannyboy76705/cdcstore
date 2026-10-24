/*
 * cdcstore-remove - take files out of a cdcstore repository
 *
 *   cdcstore-remove [-n] [-q] REPO PATTERN...
 *
 * Removes the matching entries from the index and deletes their chunk files. Every chunk belongs
 * to one file, so nothing else is affected. The chunk numbers of the other files do not change, so
 * chunk file names stay stable for whatever has already been archived or sorted.
 * PATTERN: a path (file, or folder with its contents) or a quoted wildcard. -n only shows
 * what would be removed.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#include "cdcstore-common.h"

static void usage(void) {
    fprintf(stderr,
        "Usage: cdcstore-remove [-n] [-q] REPO PATTERN...\n"
        "  -n    only show what would be removed\n"
        "  -q    quiet     -h  this help\n"
        "PATTERN: a path (file, or folder with its contents) or a quoted wildcard\n");
}

int main(int argc, char **argv) {
    int dry = 0, quiet = 0, opt;
    while ((opt = getopt(argc, argv, "nqh")) != -1) {
        switch (opt) {
            case 'n': dry = 1; break;
            case 'q': quiet = 1; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }
    if (optind + 1 >= argc) { usage(); return 1; }
    char **pats = argv + optind + 1;
    int np = argc - optind - 1;
    repo_t r;
    if (repo_open(&r, argv[optind]) != 0) return 1;
    uint64_t n = 0, bytes = 0;
    for (size_t i = 0; i < r.nen; i++) {
        entry_t *e = &r.en[i];
        if (!sel_match(pats, np, STR(&r, e->path_off))) continue;
        n++;
        if (e->type == T_FILE) bytes += e->size;
        if (!dry) e->type = T_DEL;
        else if (!quiet) printf("would remove %s\n", STR(&r, e->path_off));
    }
    char hb[32];
    human(bytes, hb, sizeof(hb));
    if (n == 0) { fprintf(stderr, "cdcstore-remove: nothing in the repository matches\n"); repo_free(&r); return 1; }
    if (dry) { fprintf(stderr, "cdcstore-remove: %llu entries (%s) would be removed; nothing changed\n", (unsigned long long)n, hb); repo_free(&r); return 0; }

    /* the chunks of the files that go; the index is saved first: a chunk file is never needed by an index that exists */
    uint32_t *gone = NULL; size_t ngone = 0;
    for (size_t i = 0; i < r.nen; i++) {
        const entry_t *e = &r.en[i];
        if (e->type != T_DEL || e->nref == 0) continue;           /* removed entries were marked just above */
        gone = realloc(gone, (ngone + e->nref) * sizeof(uint32_t));
        for (uint32_t k = 0; k < e->nref; k++) gone[ngone++] = r.refs[e->ref_start + k].chunk;
    }
    if (repo_save(&r) != 0) return 1;
    uint64_t deadbytes = 0;
    for (size_t k = 0; k < ngone; k++) deadbytes += r.ch[gone[k]].len;
    uint32_t *ids;
    size_t dead = repo_drop_chunks(&r, gone, ngone, &ids);
    if (dead && repo_save(&r) != 0) return 1;                  /* the index no longer names them ... */
    for (size_t k = 0; k < dead; k++) chunk_delete(&r, ids[k]); /* ... so the files can go */
    free(ids);
    if (!quiet) {
        char db[32];
        human(deadbytes, db, sizeof(db));
        fprintf(stderr, "cdcstore-remove: removed %llu entries (%s of file data); %llu chunk files (%s) deleted\n",
                (unsigned long long)n, hb, (unsigned long long)dead, db);
    }
    free(gone);
    repo_free(&r);
    return 0;
}
