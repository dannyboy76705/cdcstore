/* cdcstore-common.c - see cdcstore-common.h.  MIT License, Copyright (c) 2026 Daniel Lee Witzel */
#include "cdcstore-common.h"

#define GROW(p, n, cap, add) do { \
    if ((n) + (add) > (cap)) { \
        size_t nc_ = (cap) ? (cap) : 256; \
        while (nc_ < (n) + (add)) nc_ *= 2; \
        void *q_ = realloc((p), nc_ * sizeof(*(p))); \
        if (!q_) { fprintf(stderr, "cdcstore: out of memory\n"); exit(1); } \
        (p) = q_; (cap) = nc_; \
    } } while (0)

/* ---------------------------------------------------------------- hashing */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

h128_t hash128(const void *vp, size_t n) {
    const uint8_t *p = vp;
    uint64_t a = 0x9E3779B97F4A7C15ULL ^ n, b = 0xC2B2AE3D27D4EB4FULL + n * 0x165667B19E3779F9ULL;
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        a = mix64(a ^ v) + 0x9E3779B97F4A7C15ULL;
        b = mix64(b + (v ^ rotl(a, 23))) + 0xD6E8FEB86659FD93ULL;
        p += 8; n -= 8;
    }
    if (n) {
        uint64_t v = 0;
        memcpy(&v, p, n);
        v ^= (uint64_t)n << 56;
        a = mix64(a ^ v);
        b = mix64(b + (v ^ rotl(a, 23)));
    }
    h128_t h;
    h.a = mix64(a ^ rotl(b, 32));
    h.b = mix64(b + h.a);
    return h;
}
h128_t fh_start(void) { h128_t h = { 0x243F6A8885A308D3ULL, 0x13198A2E03707344ULL }; return h; }
h128_t fh_fold(h128_t acc, h128_t ch, uint32_t len) {
    acc.a = mix64(acc.a ^ ch.a) + len;
    acc.b = mix64(acc.b + ch.b) ^ (acc.a << 1);
    return acc;
}

/* ---------------------------------------------------------------- chunker */
static uint64_t gear[256];
static int gear_ready;
static void gear_init(void) {
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 256; i++) {
        s += 0x9E3779B97F4A7C15ULL;
        gear[i] = mix64(s);
    }
    gear_ready = 1;
}
uint32_t avg_round(uint32_t avg) {
    uint32_t v = 1024;
    while (v < avg && v < (1u << 22)) v <<= 1;
    if (v > avg && v > 1024 && (v - avg) > (avg - v / 2)) v >>= 1;      /* nearest power of two */
    return v;
}
void cdc_init(cdc_t *c, uint32_t avg) {
    if (!gear_ready) gear_init();
    avg = avg_round(avg);
    int k = 0;
    while ((1u << k) < avg) k++;
    c->avg = avg; c->min = avg / 4; c->max = avg * 8;
    c->ms = ((1ULL << (k + 2)) - 1) << (64 - (k + 2));
    c->ml = ((1ULL << (k - 2)) - 1) << (64 - (k - 2));
}
size_t cdc_cut(const cdc_t *c, const uint8_t *p, size_t n) {
    if (n <= c->min) return n;
    if (n > c->max) n = c->max;
    size_t normal = c->avg < n ? c->avg : n;
    uint64_t fp = 0;
    size_t i = c->min;
    for (; i < normal; i++) { fp = (fp << 1) + gear[p[i]]; if (!(fp & c->ms)) return i + 1; }
    for (; i < n; i++)      { fp = (fp << 1) + gear[p[i]]; if (!(fp & c->ml)) return i + 1; }
    return n;
}

/* ---------------------------------------------------------------- paths */
char *norm_path(const char *p, int *bad) {
    size_t l = strlen(p);
    char *o = malloc(l + 2), *w = o;
    if (!o) { fprintf(stderr, "cdcstore: out of memory\n"); exit(1); }
    if (bad) *bad = 0;
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t cl = (size_t)(p - s);
        if (cl == 0 || (cl == 1 && s[0] == '.')) continue;
        if (cl == 2 && s[0] == '.' && s[1] == '.' && bad) *bad = 1;
        if (w != o) *w++ = '/';
        memcpy(w, s, cl); w += cl;
    }
    *w = 0;
    return o;
}
int path_under(const char *base, const char *path, const char **rel) {
    size_t bl = strlen(base);
    if (bl == 0) { *rel = path; return 1; }
    if (strncmp(base, path, bl) != 0) return 0;
    if (path[bl] == 0) { *rel = path + bl; return 1; }
    if (path[bl] == '/') { *rel = path + bl + 1; return 1; }
    return 0;
}
int sel_match(char **pats, int n, const char *path) {
    if (n == 0) return 1;
    for (int i = 0; i < n; i++) {
        char *pp = norm_path(pats[i], NULL);
        int hit = 0;
        if (strpbrk(pp, "*?[")) {
            /* the path, or any folder above it, matches the wildcard */
            char buf[PATH_MAX];
            size_t l = strlen(path);
            if (l < sizeof(buf)) {
                memcpy(buf, path, l + 1);
                for (;;) {
                    if (fnmatch(pp, buf, 0) == 0) { hit = 1; break; }
                    char *sl = strrchr(buf, '/');
                    if (!sl) break;
                    *sl = 0;
                }
            }
        } else {
            size_t pl = strlen(pp);
            hit = (pl == 0) || (!strncmp(pp, path, pl) && (path[pl] == 0 || path[pl] == '/'));
        }
        free(pp);
        if (hit) return 1;
    }
    return 0;
}
int mkdir_p(const char *path, mode_t mode) {
    char buf[PATH_MAX];
    size_t l = strlen(path);
    if (l == 0 || l >= sizeof(buf)) return -1;
    memcpy(buf, path, l + 1);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
        *s = '/';
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}
void human(uint64_t v, char *out, size_t n) {
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double d = (double)v;
    int k = 0;
    while (d >= 1024 && k < 4) { d /= 1024; k++; }
    if (k == 0) snprintf(out, n, "%llu B", (unsigned long long)v);
    else snprintf(out, n, "%.2f %s", d, u[k]);
}
int ask_danger(const char *what) {
    FILE *t = fopen("/dev/tty", "r+");
    if (!t) { fprintf(stderr, "cdcstore: no terminal to ask on; use -y to go ahead without asking\n"); return 0; }
    fprintf(t, "\n*** DANGER ***\n%s\nType DELETE (capitals) to go on, anything else stops: ", what);
    fflush(t);
    char line[64] = "";
    if (!fgets(line, sizeof(line), t)) line[0] = 0;
    fclose(t);
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
    return strcmp(line, "DELETE") == 0;
}

/* ---------------------------------------------------------------- repository */
void repo_zero(repo_t *r) { memset(r, 0, sizeof(*r)); }
void repo_free(repo_t *r) {
    free(r->dir); free(r->ch); free(r->en); free(r->refs); free(r->str);
    repo_zero(r);
}
size_t str_add(repo_t *r, const char *s) {
    size_t l = strlen(s) + 1;
    if (r->nstr == 0) { GROW(r->str, r->nstr, r->capstr, 8); r->str[0] = 0; r->nstr = 8; memset(r->str, 0, 8); }
    GROW(r->str, r->nstr, r->capstr, l);
    memcpy(r->str + r->nstr, s, l);
    size_t off = r->nstr;
    r->nstr += l;
    return off;
}
entry_t *entry_new(repo_t *r) {
    GROW(r->en, r->nen, r->capen, 1);
    entry_t *e = &r->en[r->nen++];
    memset(e, 0, sizeof(*e));
    e->parent = NO_ENTRY; e->end = NO_ENTRY;
    return e;
}
void chunk_path(const repo_t *r, uint32_t id, char *out, size_t sz) {
    snprintf(out, sz, "%s/%s/b%05u/c%09u", r->dir, CHUNK_DIR, id / r->bucket, id);
}
int chunk_write(const repo_t *r, uint32_t id, const uint8_t *data, uint32_t len) {
    char p[PATH_MAX + 64];
    chunk_path(r, id, p, sizeof(p));
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 && errno == ENOENT) {
        char *sl = strrchr(p, '/');
        *sl = 0;
        mkdir_p(p, 0755);
        *sl = '/';
        fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) { fprintf(stderr, "cdcstore: cannot create %s: %s\n", p, strerror(errno)); return -1; }
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(fd, data + off, len - off);
        if (k < 0) { if (errno == EINTR) continue; fprintf(stderr, "cdcstore: write error on %s: %s\n", p, strerror(errno)); close(fd); return -1; }
        off += (size_t)k;
    }
    if (close(fd) != 0) { fprintf(stderr, "cdcstore: write error on %s: %s\n", p, strerror(errno)); return -1; }
    return 0;
}
int chunk_read(const repo_t *r, uint32_t n, uint8_t *out) {
    if (n >= r->nch) { fprintf(stderr, "cdcstore: chunk %u does not exist (damaged index)\n", n); return -1; }
    char p[PATH_MAX + 64];
    chunk_path(r, r->ch[n].id, p, sizeof(p));
    int fd = open(p, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cdcstore: cannot open %s: %s\n", p, strerror(errno)); return -1; }
    size_t off = 0, len = r->ch[n].len;
    while (off < len) {
        ssize_t k = read(fd, out + off, len - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        off += (size_t)k;
    }
    char x;
    int longer = off == len && read(fd, &x, 1) > 0;
    close(fd);
    if (off != len || longer) { fprintf(stderr, "cdcstore: %s has the wrong size (should be %zu bytes)\n", p, len); return -1; }
    return 0;
}
int chunk_delete(const repo_t *r, uint32_t id) {
    char p[PATH_MAX + 64];
    chunk_path(r, id, p, sizeof(p));
    int rc = unlink(p);
    if (rc == 0) {                       /* an emptied bucket folder goes too */
        char *sl = strrchr(p, '/');
        *sl = 0;
        rmdir(p);
    }
    return rc;
}
size_t repo_drop_chunks(repo_t *r, const uint32_t *rows, size_t n, uint32_t **ids) {
    uint8_t *drop = calloc(r->nch + 1, 1);
    uint32_t *map = malloc((r->nch + 1) * sizeof(uint32_t));
    uint32_t *out = malloc((n + 1) * sizeof(uint32_t));
    if (!drop || !map || !out) { fprintf(stderr, "cdcstore: out of memory\n"); exit(1); }
    size_t nd = 0;
    for (size_t i = 0; i < n; i++)
        if (rows[i] < r->nch && !drop[rows[i]]) { drop[rows[i]] = 1; out[nd++] = r->ch[rows[i]].id; }
    size_t w = 0;
    for (size_t i = 0; i < r->nch; i++) {
        if (drop[i]) { map[i] = NO_ENTRY; continue; }
        map[i] = (uint32_t)w;
        r->ch[w++] = r->ch[i];
    }
    r->nch = w;
    for (size_t k = 0; k < r->nrefs; k++) r->refs[k].chunk = map[r->refs[k].chunk];
    free(drop); free(map);
    *ids = out;
    return nd;
}

size_t chunk_insert(repo_t *r, const chunk_t *c) {
    GROW(r->ch, r->nch, r->capch, 1);
    r->ch[r->nch] = *c;
    return r->nch++;
}

static int key_of(unsigned char c) { return c == 0 ? 0 : c == '/' ? 1 : c + 2; }
static int path_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = key_of((unsigned char)*a), y = key_of((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!*a) return 0;
    }
}
static const char *g_str;
static int ent_cmp(const void *pa, const void *pb) {
    const entry_t *a = pa, *b = pb;
    return path_cmp(g_str + a->path_off, g_str + b->path_off);
}
long entry_find(const repo_t *r, const char *path) {
    size_t lo = 0, hi = r->nen_loaded;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = path_cmp(STR(r, r->en[mid].path_off), path);
        if (c == 0) return r->en[mid].type == T_DEL ? -1 : (long)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

static int is_under(const char *dir, const char *path) {
    size_t l = strlen(dir);
    return strncmp(dir, path, l) == 0 && path[l] == '/';
}

/* drop deleted entries, add missing parent folders, sort, set parent/end, repack refs and strings */
static void repo_finalize(repo_t *r) {
    for (;;) {
        size_t w = 0;
        for (size_t i = 0; i < r->nen; i++) if (r->en[i].type != T_DEL) r->en[w++] = r->en[i];
        r->nen = w;
        g_str = r->str;
        qsort(r->en, r->nen, sizeof(entry_t), ent_cmp);
        w = 0;                                                   /* same path twice: keep the first */
        for (size_t i = 0; i < r->nen; i++) {
            if (w && path_cmp(STR(r, r->en[w - 1].path_off), STR(r, r->en[i].path_off)) == 0) continue;
            r->en[w++] = r->en[i];
        }
        r->nen = w;
        size_t *stack = malloc((r->nen + 1) * sizeof(size_t)), sp = 0;
        char **missing = NULL; size_t nmiss = 0, capmiss = 0;
        for (size_t i = 0; i < r->nen; i++) {
            const char *path = STR(r, r->en[i].path_off);
            while (sp && !is_under(STR(r, r->en[stack[sp - 1]].path_off), path)) sp--;
            const char *sl = strrchr(path, '/');
            if (sl) {
                size_t pl = (size_t)(sl - path);
                const char *top = sp ? STR(r, r->en[stack[sp - 1]].path_off) : NULL;
                if (!(top && strlen(top) == pl && !memcmp(top, path, pl))) {
                    char *m = strndup(path, pl);
                    if (nmiss && !strcmp(missing[nmiss - 1], m)) free(m);
                    else { GROW(missing, nmiss, capmiss, 1); missing[nmiss++] = m; }
                }
            }
            if (r->en[i].type == T_DIR) stack[sp++] = i;
        }
        free(stack);
        if (!nmiss) { free(missing); break; }
        for (size_t i = 0; i < nmiss; i++) {
            size_t off = str_add(r, missing[i]);
            entry_t *e = entry_new(r);
            e->path_off = off; e->type = T_DIR; e->mode = 040755; e->mtime = (int64_t)time(NULL);
            free(missing[i]);
        }
        free(missing);
    }
    /* parent and end */
    size_t *stack = malloc((r->nen + 1) * sizeof(size_t)), sp = 0;
    for (size_t i = 0; i < r->nen; i++) {
        const char *path = STR(r, r->en[i].path_off);
        while (sp && !is_under(STR(r, r->en[stack[sp - 1]].path_off), path)) { r->en[stack[sp - 1]].end = (uint32_t)i; sp--; }
        r->en[i].parent = sp ? (uint32_t)stack[sp - 1] : NO_ENTRY;
        if (r->en[i].type == T_DIR) { stack[sp++] = i; r->en[i].end = NO_ENTRY; }
        else r->en[i].end = (uint32_t)(i + 1);
    }
    while (sp) r->en[stack[--sp]].end = (uint32_t)r->nen;
    free(stack);
    /* refs and strings, in entry order */
    ref_t *nr = malloc((r->nrefs + 1) * sizeof(ref_t));
    char *ns = malloc(r->nstr + 16);
    if (!nr || !ns) { fprintf(stderr, "cdcstore: out of memory\n"); exit(1); }
    size_t nrn = 0, nsn = 8;
    memset(ns, 0, 8);
    for (size_t i = 0; i < r->nen; i++) {
        entry_t *e = &r->en[i];
        if (e->type == T_FILE && e->nref) {
            memcpy(nr + nrn, r->refs + e->ref_start, e->nref * sizeof(ref_t));
            e->ref_start = nrn; nrn += e->nref;
        } else e->ref_start = 0;
        const char *s = STR(r, e->path_off);
        size_t l = strlen(s) + 1;
        memcpy(ns + nsn, s, l); e->path_off = nsn; nsn += l;
        if (e->type == T_LINK) {
            s = STR(r, e->aux_off); l = strlen(s) + 1;
            memcpy(ns + nsn, s, l); e->aux_off = nsn; nsn += l;
        } else e->aux_off = 0;
    }
    free(r->refs); free(r->str);
    r->refs = nr; r->nrefs = nrn; r->capref = r->nrefs + 1;
    r->str = ns; r->nstr = nsn; r->capstr = nsn + 16;
    r->nen_loaded = r->nen;
}

static size_t al8(size_t v) { return (v + 7) & ~(size_t)7; }

int repo_save(repo_t *r) {
    repo_finalize(r);
    idxhdr_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "CDCSTOR1", 8);
    h.version = 1; h.avg = r->avg; h.whole = r->whole; h.bucket = r->bucket; h.next_id = r->next_id;
    h.nch = r->nch; h.nen = r->nen; h.nrefs = r->nrefs; h.nstr = r->nstr;
    size_t o = al8(sizeof(h));
    h.off_ch = o;   o = al8(o + r->nch * sizeof(chunk_t));
    h.off_en = o;   o = al8(o + r->nen * sizeof(entry_t));
    h.off_refs = o; o = al8(o + r->nrefs * sizeof(ref_t));
    h.off_str = o;  o = al8(o + r->nstr);
    h.total = o;
    uint8_t *buf = calloc(1, o);
    if (!buf) { fprintf(stderr, "cdcstore: out of memory\n"); return -1; }
    memcpy(buf, &h, sizeof(h));
    if (r->nch)   memcpy(buf + h.off_ch, r->ch, r->nch * sizeof(chunk_t));
    if (r->nen)   memcpy(buf + h.off_en, r->en, r->nen * sizeof(entry_t));
    if (r->nrefs) memcpy(buf + h.off_refs, r->refs, r->nrefs * sizeof(ref_t));
    memcpy(buf + h.off_str, r->str, r->nstr);
    h.checksum = hash128(buf, o).a;
    memcpy(buf, &h, sizeof(h));
    char tmp[PATH_MAX], fin[PATH_MAX];
    snprintf(fin, sizeof(fin), "%s/%s", r->dir, IDX_NAME);
    snprintf(tmp, sizeof(tmp), "%s/%s.tmp", r->dir, IDX_NAME);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc = -1;
    if (fd >= 0) {
        size_t off = 0;
        while (off < o) {
            ssize_t w = write(fd, buf + off, o - off);
            if (w < 0) { if (errno == EINTR) continue; break; }
            off += (size_t)w;
        }
        if (off == o && fsync(fd) == 0) rc = 0;
        close(fd);
    }
    free(buf);
    if (rc == 0 && rename(tmp, fin) != 0) rc = -1;
    if (rc != 0) fprintf(stderr, "cdcstore: cannot write the index %s: %s\n", fin, strerror(errno));
    return rc;
}

int repo_create(repo_t *r, const char *dir, uint32_t avg, uint32_t whole) {
    repo_zero(r);
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", dir, CHUNK_DIR);
    if (mkdir_p(p, 0755) != 0) { fprintf(stderr, "cdcstore: cannot make %s: %s\n", p, strerror(errno)); return -1; }
    r->dir = strdup(dir);
    r->avg = avg_round(avg);
    r->whole = whole;
    r->bucket = DEF_BUCKET; r->next_id = 0;
    return 0;
}

static int read_all(const char *path, uint8_t **out, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    uint8_t *b = malloc((size_t)st.st_size + 1);
    if (!b) { close(fd); return -1; }
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t n = read(fd, b + off, (size_t)st.st_size - off);
        if (n < 0) { if (errno == EINTR) continue; free(b); close(fd); return -1; }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    *out = b; *len = off;
    return 0;
}

int repo_open(repo_t *r, const char *dir) {
    repo_zero(r);
    char p[PATH_MAX + 64], use[PATH_MAX];
    snprintf(use, sizeof(use), "%s", dir);
    snprintf(p, sizeof(p), "%s/%s", use, IDX_NAME);
    if (access(p, R_OK) != 0) {
        snprintf(use, sizeof(use), "%s/%s", dir, INPLACE_DIR);
        snprintf(p, sizeof(p), "%s/%s", use, IDX_NAME);
    }
    uint8_t *buf; size_t len;
    if (read_all(p, &buf, &len) != 0) {
        fprintf(stderr, "cdcstore: no repository in %s (cannot read %s/%s or %s/%s/%s): %s\n", dir, dir, IDX_NAME, dir, INPLACE_DIR, IDX_NAME, strerror(errno));
        return -1;
    }
    idxhdr_t h;
    if (len < sizeof(h)) goto bad;
    memcpy(&h, buf, sizeof(h));
    if (memcmp(h.magic, "CDCSTOR1", 8) || h.version != 1) { fprintf(stderr, "cdcstore: %s is not a cdcstore index (or a newer version)\n", p); free(buf); return -1; }
    if (h.total != len) goto bad;
    {
        uint64_t sum = h.checksum;
        idxhdr_t z = h; z.checksum = 0;
        memcpy(buf, &z, sizeof(z));
        uint64_t got = hash128(buf, len).a;
        memcpy(buf, &h, sizeof(h));
        if (got != sum) { fprintf(stderr, "cdcstore: %s fails its checksum (damaged)\n", p); free(buf); return -1; }
    }
    if (h.off_ch + h.nch * sizeof(chunk_t) > len || h.bucket == 0 ||
        h.off_en + h.nen * sizeof(entry_t) > len || h.off_refs + h.nrefs * sizeof(ref_t) > len || h.off_str + h.nstr > len) goto bad;
    r->dir = strdup(use);
    r->avg = h.avg; r->whole = h.whole; r->bucket = h.bucket; r->next_id = h.next_id;
#define LOADARR(f, cnt, off, T) do { r->f = malloc(((cnt) + 1) * sizeof(T)); if (!r->f) { free(buf); return -1; } \
    if (cnt) memcpy(r->f, buf + (off), (cnt) * sizeof(T)); r->n##f = (cnt); r->cap##f = (cnt) + 1; } while (0)
    r->nch = h.nch; r->capch = h.nch + 1; r->ch = malloc((h.nch + 1) * sizeof(chunk_t)); if (h.nch) memcpy(r->ch, buf + h.off_ch, h.nch * sizeof(chunk_t));
    r->nen = h.nen; r->capen = h.nen + 1; r->en = malloc((h.nen + 1) * sizeof(entry_t)); if (h.nen) memcpy(r->en, buf + h.off_en, h.nen * sizeof(entry_t));
    r->nrefs = h.nrefs; r->capref = h.nrefs + 1; r->refs = malloc((h.nrefs + 1) * sizeof(ref_t)); if (h.nrefs) memcpy(r->refs, buf + h.off_refs, h.nrefs * sizeof(ref_t));
    r->nstr = h.nstr; r->capstr = h.nstr + 16; r->str = malloc(h.nstr + 16); memcpy(r->str, buf + h.off_str, h.nstr);
#undef LOADARR
    free(buf);
    if (!r->ch || !r->en || !r->refs || !r->str) { fprintf(stderr, "cdcstore: out of memory\n"); return -1; }
    r->nen_loaded = r->nen;
    return 0;
bad:
    fprintf(stderr, "cdcstore: %s is damaged (sizes do not add up)\n", p);
    free(buf);
    return -1;
}

/* ---------------------------------------------------------------- verify */
uint64_t repo_verify(const repo_t *r, char **pats, int npats, const char *cmpbase, uint64_t *nfiles, uint64_t *nbytes) {
    uint64_t bad = 0, nf = 0, nb = 0;
    uint32_t maxc = r->avg * 8;
    if (r->whole > maxc) maxc = r->whole;
    uint8_t *tmp = malloc(maxc), *cmp = malloc(maxc);
    if (!tmp || !cmp) { fprintf(stderr, "cdcstore: out of memory\n"); exit(1); }
    for (size_t i = 0; i < r->nen; i++) {
        const entry_t *e = &r->en[i];
        const char *path = STR(r, e->path_off);
        if (e->type != T_FILE || !sel_match(pats, npats, path)) continue;
        nf++;
        int fd = -1;
        if (cmpbase) {
            char op[PATH_MAX + 64];
            snprintf(op, sizeof(op), "%s/%s", cmpbase, path);
            fd = open(op, O_RDONLY);
        }
        h128_t fh = fh_start();
        uint64_t total = 0;
        int ok = 1;
        for (uint32_t k = 0; k < e->nref && ok; k++) {
            const ref_t *rf = &r->refs[e->ref_start + k];
            if (rf->chunk >= r->nch || rf->off != total) { fprintf(stderr, "cdcstore: %s: the index is inconsistent at chunk %u\n", path, k); ok = 0; break; }
            const chunk_t *c = &r->ch[rf->chunk];
            if (c->len > maxc || chunk_read(r, rf->chunk, tmp) != 0) { fprintf(stderr, "cdcstore: %s: chunk %u cannot be read\n", path, k); ok = 0; break; }
            h128_t h = hash128(tmp, c->len);
            if (h.a != c->h0 || h.b != c->h1) { fprintf(stderr, "cdcstore: %s: chunk %u does not match its hash (the chunk file is damaged)\n", path, k); ok = 0; break; }
            fh = fh_fold(fh, h, c->len);
            if (fd >= 0) {
                size_t got = 0;
                while (got < c->len) {
                    ssize_t n = read(fd, cmp + got, c->len - got);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                if (got != c->len || memcmp(cmp, tmp, c->len) != 0) { fprintf(stderr, "cdcstore: %s: differs from the original at byte %llu\n", path, (unsigned long long)total); ok = 0; break; }
            }
            total += c->len;
        }
        if (ok && (total != e->size || fh.a != e->fh0 || fh.b != e->fh1)) { fprintf(stderr, "cdcstore: %s: size or file hash does not match the index\n", path); ok = 0; }
        if (ok && fd >= 0) {
            char x;
            if (read(fd, &x, 1) > 0) { fprintf(stderr, "cdcstore: %s: the original is longer than what is stored\n", path); ok = 0; }
        }
        if (fd >= 0) close(fd);
        if (!ok) bad++;
        nb += total;
    }
    free(tmp); free(cmp);
    if (nfiles) *nfiles = nf;
    if (nbytes) *nbytes = nb;
    return bad;
}
