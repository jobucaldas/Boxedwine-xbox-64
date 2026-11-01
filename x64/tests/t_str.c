/* t_str.c - string and memory conformance: memcpy/memmove/memset/memcmp and
 * the str* family over many sizes, alignments and overlaps (self-checked
 * against naive references), qsort/bsearch, snprintf/sscanf formats,
 * strtol/strtoull bases, ctype.
 *
 * Single-threaded and deterministic. Pointers are never printed (only offsets).
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL
#define BUFSZ 1024

static uint64_t hash;
static unsigned long long mismatches;
static uint64_t lcg = 0x243F6A8885A308D3ULL;

static uint32_t rnd(void) {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)(lcg >> 33);
}

static void mix8(uint64_t v) {
    hash ^= (uint8_t)v;
    hash *= FNV_PRIME;
}
static void mixbytes(const void *p, size_t n) {
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++) mix8(c[i]);
}
static void mixi(long long v) {
    for (int i = 0; i < 8; i++) mix8((uint64_t)v >> (i * 8));
}
static void check(int ok) {
    if (!ok) mismatches++;
}
static void begin(void) {
    hash = FNV_OFFSET;
    mismatches = 0;
}
static void end(const char *name) {
    printf("%-12s %016llx mismatches=%llu\n", name, (unsigned long long)hash, mismatches);
}

static unsigned char src[BUFSZ] __attribute__((aligned(64)));
static unsigned char dst[BUFSZ] __attribute__((aligned(64)));
static unsigned char tmp[BUFSZ];

/* naive references */
static long ref_strchr(const char *s, int c) {
    for (long i = 0;; i++) {
        if (s[i] == (char)c) return i;
        if (!s[i]) return -1;
    }
}
static long ref_strrchr(const char *s, int c) {
    long last = -1;
    for (long i = 0;; i++) {
        if (s[i] == (char)c) last = i;
        if (!s[i]) return last;
    }
}
static long ref_strstr(const char *h, const char *n) {
    size_t nl = strlen(n), hl = strlen(h);
    for (size_t i = 0; i + nl <= hl; i++) {
        if (memcmp(h + i, n, nl) == 0) return (long)i;
    }
    return -1;
}
static int sgn(int v) { return (v > 0) - (v < 0); }

static void sec_mem(void) {
    begin();
    for (size_t i = 0; i < BUFSZ; i++) src[i] = (unsigned char)rnd();

    /* memcpy: every length 0..300, source offset 0..15, dest offset 0..15 */
    for (size_t n = 0; n <= 300; n++)
        for (size_t so = 0; so < 16; so++)
            for (size_t dof = 0; dof < 16; dof++) {
                memset(dst, (int)(0x5a ^ (n + dof)), 320);
                memcpy(dst + dof, src + so, n);
                check(memcmp(dst + dof, src + so, n) == 0);
                mixbytes(dst, 320);
            }
    end("memcpy");

    /* memmove with overlap in both directions */
    hash = FNV_OFFSET;
    mismatches = 0;
    for (int d = -20; d <= 20; d++)
        for (size_t n = 0; n <= 300; n++) {
            unsigned char save[BUFSZ];
            memcpy(save, src, BUFSZ);
            unsigned char *base = src + 320;
            memmove(base + d, base, n);
            memcpy(tmp, save + 320, n);
            /* expected: the region at base+d holds the original bytes at base */
            int ok = 1;
            for (size_t i = 0; i < n; i++)
                if (base[d + (long)i] != tmp[i]) ok = 0;
            check(ok);
            mixbytes(src + 300, 40);
            memcpy(src, save, BUFSZ); /* restore */
        }
    end("memmove");

    /* memset with varying value/length/offset */
    hash = FNV_OFFSET;
    mismatches = 0;
    for (size_t n = 0; n <= 300; n++)
        for (size_t off = 0; off < 16; off++) {
            int c = (int)((n * 7 + off) & 0xff);
            memset(dst + off, c, n);
            int ok = 1;
            for (size_t i = 0; i < n; i++)
                if (dst[off + i] != (unsigned char)c) ok = 0;
            check(ok);
            mixbytes(dst, 320);
        }
    end("memset");

    /* memcmp and memchr: sign and found position */
    hash = FNV_OFFSET;
    mismatches = 0;
    for (size_t n = 0; n <= 300; n++)
        for (size_t off = 0; off < 16; off++) {
            unsigned char a[330], b[330];
            for (size_t i = 0; i < 330; i++) a[i] = (unsigned char)rnd();
            memcpy(b, a, 330);
            int r0 = memcmp(a + off, b + off, n);
            check(r0 == 0);
            size_t p = n ? rnd() % n : 0;
            if (n) {
                b[off + p] ^= 0x80;
                int r1 = memcmp(a + off, b + off, n);
                int ref = a[off + p] < b[off + p] ? -1 : 1;
                check(sgn(r1) == ref);
                mix8(sgn(r1) + 2);
                /* first occurrence of a[p] in b, checked against a naive scan */
                long ref_pos = -1;
                for (size_t i = 0; i < n; i++)
                    if (b[off + i] == a[off + p]) {
                        ref_pos = (long)i;
                        break;
                    }
                const void *f = memchr(b + off, a[off + p], n);
                long got = f ? (long)((const unsigned char *)f - (b + off)) : -1;
                check(got == ref_pos);
                mixi(got);
            }
        }
    end("memcmp");
}

static void sec_str(void) {
    static const char set[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    static char buf[16 + 320 + 16];
    static char copy[16 + 320 + 16];
    static char dstb[16 + 320 + 16];
    begin();
    for (size_t L = 0; L <= 300; L++)
        for (size_t o = 0; o < 16; o++) {
            char *s = buf + o;
            for (size_t i = 0; i < L; i++) s[i] = set[rnd() % (sizeof set - 2)];
            s[L] = 0;
            check(strlen(s) == L);

            /* the search character is taken from the string or is absent ('!') */
            if (L) {
                int c1 = (unsigned char)s[L / 2];
                const char *p1 = strchr(s, c1);
                check(p1 && (p1 - s) == ref_strchr(s, c1));
                mixi(p1 ? (long long)(p1 - s) : -1);
                const char *p2 = strrchr(s, c1);
                check(p2 && (p2 - s) == ref_strrchr(s, c1));
                mixi(p2 ? (long long)(p2 - s) : -1);
            }
            check(strchr(s, '!') == NULL && strrchr(s, '!') == NULL);
            check(strchr(s, 0) == s + L); /* strchr(s, 0) finds the terminator */

            /* substring search: needle from the middle of the haystack */
            if (L >= 3) {
                size_t a = rnd() % (L - 2);
                size_t k = 1 + rnd() % 4;
                if (a + k > L) k = L - a;
                char needle[8];
                memcpy(needle, s + a, k);
                needle[k] = 0;
                const char *p3 = strstr(s, needle);
                check(p3 && (p3 - s) == ref_strstr(s, needle));
                mixi(p3 ? (long long)(p3 - s) : -1);
            }
            check(strstr(s, "!#") == NULL);

            /* strcmp / strncmp against a copy with one changed byte */
            strcpy(copy + o, s);
            check(strcmp(s, copy + o) == 0);
            if (L) {
                size_t q = rnd() % L;
                copy[o + q] = (char)(s[q] + 1);
                int r = strcmp(s, copy + o);
                check(sgn(r) == -1);
                mix8(sgn(r) + 2);
                check(sgn(strncmp(s, copy + o, q)) == 0);
                check(sgn(strncmp(s, copy + o, q + 1)) == -1);
                copy[o + q] = s[q];
            }
            check(strncmp(s, copy + o, L + 5) == 0);

            /* strcpy / strncpy / strcat into a separate buffer */
            char *d = dstb + o;
            strcpy(d, s);
            check(strlen(d) == L);
            memset(dstb + o + L + 1, '#', 20);
            strncpy(d, "xy", 2);
            check(d[0] == 'x' && d[1] == 'y');
            strcpy(d, s);
            strcat(d, "!");
            check(strlen(d) == L + 1 && d[L] == '!');
            mixbytes(dstb, 16 + L + 4);

            /* span/break against a fixed set */
            mixi((long long)strspn(s, "abcdef"));
            mixi((long long)strcspn(s, "0123456789"));
            const char *pb = strpbrk(s, "XYZ");
            mixi(pb ? (long long)(pb - s) : -1);
        }
    end("str");
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sec_sort(void) {
    begin();
    enum { N = 5000 };
    static int arr[N];
    static unsigned char present[3 * N + 16];
    for (int i = 0; i < N; i++) arr[i] = 3 * i + (int)(rnd() % 3);
    /* Fisher-Yates shuffle */
    for (int i = N - 1; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        int t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
    }
    for (int i = 0; i < N; i++) present[arr[i]] = 1;
    qsort(arr, N, sizeof arr[0], cmp_int);
    int sorted = 1;
    for (int i = 1; i < N; i++)
        if (arr[i - 1] > arr[i]) sorted = 0;
    check(sorted);
    for (int i = 0; i < N; i++) mixi(arr[i]);

    int found = 0;
    for (int q = 0; q < 3000; q++) {
        int key = (int)(rnd() % (3 * N + 10));
        int *hit = bsearch(&key, arr, N, sizeof arr[0], cmp_int);
        int ref = key < 3 * N + 16 && present[key];
        check((hit != NULL) == (ref != 0));
        if (hit) {
            found++;
            mixi(hit - arr);
            check(*hit == key);
        } else {
            mixi(-1);
        }
    }
    mixi(found);
    printf("qsort ints: found=%d first=%d last=%d\n", found, arr[0], arr[N - 1]);
    end("qsort_int");

    begin();
    enum { S = 400 };
    static char words[S][24];
    static char *ptrs[S];
    for (int i = 0; i < S; i++) {
        int len = 1 + (int)(rnd() % 20);
        for (int j = 0; j < len; j++) words[i][j] = (char)('a' + rnd() % 26);
        words[i][len] = 0;
        ptrs[i] = words[i];
    }
    qsort(ptrs, S, sizeof ptrs[0], cmp_str);
    int ok = 1;
    for (int i = 1; i < S; i++)
        if (strcmp(ptrs[i - 1], ptrs[i]) > 0) ok = 0;
    check(ok);
    for (int i = 0; i < S; i++) mixbytes(ptrs[i], strlen(ptrs[i]) + 1);
    end("qsort_str");
}

static void sec_printf(void) {
    char b[256];
    int n;
    printf("--- snprintf ---\n");
    snprintf(b, sizeof b, "%d|%i|%u|%x|%X|%o|%c|%s|%%", -42, 7, 4000000000u, 0xbeefu, 0xbeefu, 0777u, 'Q', "str");
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%+08d][%-6d][% d][%06.2f][%+.3e]", 42, -17, 5, 3.14159, 12345.678);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%#x][%#o][%#X][%.3s][%10.4f][%-12.3e]", 255u, 8u, 0xabu, "abcdef", 2.5, 0.000123);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%G][%g][%g][%g][%g][%e]", 1e-5, 123456789.0, 0.0001, 1e100, 100000.0, 0.0);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%a][%A][%.0f][%.0f][%.0f][%.1f]", 1.5, 0.75, 0.5, 1.5, 2.5, 0.25);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%lld][%llu][%zu][%hhd][%hd][%ld][%jd][%td][%hhu]", -9000000000LL,
             18000000000000000000ULL, (size_t)123, 300, 70000, -5L, (intmax_t)-7, (ptrdiff_t)88, 257u);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%*d][%-*d][%.*s][%5c][%05.1f][%.20g]", 7, 42, 5, 9, 3, "abcdef", 'z', 9.96, 0.1);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%lf][%Lf][%f][%f]", 1.0 / 3.0, 2.0L / 3.0L, 1e15, -0.0);
    printf("%s\n", b);
    snprintf(b, sizeof b, "[%s][%.2s][%5s][%-5s]", "", "x", "ab", "ab");
    printf("%s\n", b);
    n = 0;
    snprintf(b, sizeof b, "abc%n", &n);
    printf("n=%d b=%s\n", n, b);
    n = snprintf(NULL, 0, "%d-%d", 12345, 678);
    printf("snprintf len=%d\n", n);
    n = snprintf(b, 4, "truncate-me");
    printf("truncated ret=%d buf=%s\n", n, b);

    /* hash many generated formatted lines */
    begin();
    for (int i = 0; i < 3000; i++) {
        int k = snprintf(b, sizeof b, "%d:%x:%.3f:%5.2e:%-8s|%lld", i - 1500, (unsigned)(i * 2654435761u),
                         i / 7.0, i * 1.5e-3, i % 2 ? "odd" : "even", (long long)i * 1234567890LL);
        mixbytes(b, (size_t)k);
    }
    end("snprintf");

    printf("--- sscanf ---\n");
    int di, di2;
    unsigned ux;
    long lo = 0;
    char word[32], word2[32];
    double df;
    float ff;
    int consumed = -1;
    int r = sscanf("  42 0x1F -17 abc 3.5e2 1.25 xyz", "%d %x %d %31s %lf %f %n", &di, &ux, &di2, word, &df, &ff,
                   &consumed);
    printf("sscanf r=%d di=%d ux=%u di2=%d word=%s df=%g ff=%g consumed=%d\n", r, di, ux, di2, word, df,
           (double)ff, consumed);
    r = sscanf("key=value;rest", "%31[a-z]=%31[^;];%ld", word, word2, &lo);
    printf("sscanf r=%d word=%s word2=%s lo=%ld\n", r, word, word2, lo);
    char ch1, ch2;
    r = sscanf("A B", "%c %c", &ch1, &ch2);
    printf("sscanf r=%d ch=%c%c\n", r, ch1, ch2);
    r = sscanf("12345678901234567890", "%ld", &lo);
    printf("sscanf overflow r=%d lo=%ld\n", r, lo);
    r = sscanf("nope", "%d", &di);
    printf("sscanf fail r=%d\n", r);
    r = sscanf("width 123456", "%*s %3d", &di);
    printf("sscanf skip r=%d di=%d\n", r, di);
    unsigned char uc;
    r = sscanf("300 -1", "%hhu %hhd", &uc, (signed char *)&ch1);
    printf("sscanf hh r=%d uc=%u ch=%d\n", r, uc, (int)(signed char)ch1);
    long long ll;
    r = sscanf("-9223372036854775808", "%lld", &ll);
    printf("sscanf lld r=%d ll=%lld\n", r, ll);
}

static void sec_strto(void) {
    static const char *inputs[] = {"0x7fff", "-0x80", "0777", "  -12abc", "+42", "z", "35z", "99999999999999999999",
                                   "-9223372036854775808", "18446744073709551615", "18446744073709551616",
                                   "0", "-0", "1010", "0b101", "ZZ"};
    static const int bases[] = {0, 2, 8, 10, 16, 36};
    for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
        for (unsigned j = 0; j < sizeof bases / sizeof bases[0]; j++) {
            char *end;
            errno = 0;
            long v = strtol(inputs[i], &end, bases[j]);
            int e1 = errno;
            errno = 0;
            unsigned long long u = strtoull(inputs[i], NULL, bases[j]);
            int e2 = errno;
            printf("%-22s b%-2d sl=%ld u=%llu off=%ld e=%d/%d\n", inputs[i], bases[j], v, u,
                   (long)(end - inputs[i]), e1, e2);
        }
    printf("strtol bad base errno: ");
    errno = 0;
    long bb = strtol("12", NULL, 1);
    printf("%ld %d\n", bb, errno);
}

static void sec_ctype(void) {
    begin();
    int up = 0, lo = 0, al = 0, dg = 0, sp = 0, xd = 0, pr = 0;
    for (int c = -1; c <= 255; c++) {
        int ci = c < 0 ? EOF : c;
        mix8((uint64_t)(toupper(ci) & 0xff));
        mix8((uint64_t)(tolower(ci) & 0xff));
        up += isupper(ci) != 0;
        lo += islower(ci) != 0;
        al += isalpha(ci) != 0;
        dg += isdigit(ci) != 0;
        sp += isspace(ci) != 0;
        xd += isxdigit(ci) != 0;
        pr += isprint(ci) != 0;
    }
    printf("ctype counts: upper=%d lower=%d alpha=%d digit=%d space=%d xdigit=%d print=%d\n", up, lo, al, dg, sp,
           xd, pr);
    end("ctype");
}

int main(void) {
    printf("t_str: string and memory conformance\n");
    sec_mem();
    sec_str();
    sec_sort();
    sec_printf();
    sec_strto();
    sec_ctype();
    printf("t_str done\n");
    return 0;
}
