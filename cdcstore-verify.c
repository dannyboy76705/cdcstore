/*
 * cdcstore-verify - check a cdcstore repository
 *
 *   cdcstore-verify [-q] [-c DIR] REPO [PATTERN...]
 *
 * Reads the selected files back out of the chunk files: every chunk file must have its length and match its hash,
 * sizes and file hashes must match the index. With -c DIR each file is also compared byte for byte
 * with the original at DIR/PATH (originals that no longer exist are skipped).
 * Exit status 0 = all good, 2 = problems found.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#include "cdcstore-common.h"

static void usage(void) {
    fprintf(stderr,
        "Usage: cdcstore-verify [-q] [-c DIR] REPO [PATTERN...]\n"
        "  -c DIR  also compare each file with the original under DIR\n"
        "  -q      quiet (problems are still reported)     -h  this help\n"
        "PATTERN: a path (file, or folder with its contents) or a quoted wildcard; none = everything\n");
}

int main(int argc, char **argv) {
    const char *cmp = NULL;
    int quiet = 0, opt;
    while ((opt = getopt(argc, argv, "c:qh")) != -1) {
        switch (opt) {
            case 'c': cmp = optarg; break;
            case 'q': quiet = 1; break;
            case 'h': usage(); return 0;
            default: usage(); return 1;
        }
    }
    if (optind >= argc) { usage(); return 1; }
    repo_t r;
    if (repo_open(&r, argv[optind]) != 0) return 1;
    uint64_t nf, nb;
    uint64_t bad = repo_verify(&r, argv + optind + 1, argc - optind - 1, cmp, &nf, &nb);
    if (!quiet || bad) {
        char hb[32];
        human(nb, hb, sizeof(hb));
        fprintf(stderr, "cdcstore-verify: %llu files (%s) read%s: %s\n", (unsigned long long)nf, hb, cmp ? " and compared" : "",
                bad ? "PROBLEMS FOUND" : "all good");
        if (bad) fprintf(stderr, "cdcstore-verify: %llu file(s) with problems\n", (unsigned long long)bad);
    }
    repo_free(&r);
    return bad ? 2 : 0;
}
