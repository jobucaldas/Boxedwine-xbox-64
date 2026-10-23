/* t_int.c - x86-64 integer conformance: arithmetic and overflow, signed/unsigned
 * division (incl. negative operands), 128-bit multiply/divide, shifts, rotates,
 * rcl/rcr, shld/shrd, bit scan/test, bswap, adc/sbb, neg, setcc, lahf/sahf,
 * cmov, cmpxchg/cmpxchg16b, xadd/xchg.
 *
 * Single-threaded and deterministic. Every section prints a checksum of its
 * results plus "mismatches=N", where N counts self-checks of the inline asm
 * against a portable C reference. Native runs must show mismatches=0.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t hash;
static unsigned long long mismatches;
static uint64_t rng = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd64(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void mix8(uint64_t v) {
    hash ^= (uint8_t)v;
    hash *= FNV_PRIME;
}
static void mix16(uint64_t v) { mix8(v); mix8(v >> 8); }
static void mix32(uint64_t v) { mix16(v); mix16(v >> 16); }
static void mix64(uint64_t v) { mix32(v); mix32(v >> 32); }

static void begin(void) {
    hash = FNV_OFFSET;
    mismatches = 0;
}

static void end(const char *name) {
    printf("%-12s %016llx mismatches=%llu\n", name, (unsigned long long)hash, mismatches);
}

static void check(int ok) {
    if (!ok) mismatches++;
}

static uint64_t mask_w(unsigned w) {
    return w == 64 ? ~0ULL : ((1ULL << w) - 1);
}

/* ---- inline asm helpers ------------------------------------------------ */

#define DEF_ROT(NAME, T, OP, SFX)                                             \
    static T NAME(T v, uint8_t n) {                                           \
        __asm__(OP SFX " %%cl, %[v]" : [v] "+r"(v) : "c"(n) : "cc");          \
        return v;                                                             \
    }
DEF_ROT(rol8, uint8_t, "rol", "b")
DEF_ROT(rol16, uint16_t, "rol", "w")
DEF_ROT(rol32, uint32_t, "rol", "l")
DEF_ROT(rol64, uint64_t, "rol", "q")
DEF_ROT(ror8, uint8_t, "ror", "b")
DEF_ROT(ror16, uint16_t, "ror", "w")
DEF_ROT(ror32, uint32_t, "ror", "l")
DEF_ROT(ror64, uint64_t, "ror", "q")

/* rcl/rcr: carry in via "negb" (CF = cf_in), carry out via setc. */
#define DEF_ROT_CF(NAME, T, OP, SFX)                                          \
    static T NAME(T v, uint8_t n, int cf_in, int *cf_out) {                   \
        unsigned char c = (unsigned char)(cf_in != 0), out;                   \
        __asm__ volatile("negb %b[c]\n\t" OP SFX " %%cl, %[v]\n\t"            \
                         "setc %[o]"                                          \
                         : [v] "+r"(v), [c] "+q"(c), [o] "=q"(out)            \
                         : "c"(n)                                             \
                         : "cc");                                             \
        *cf_out = out;                                                        \
        return v;                                                             \
    }
DEF_ROT_CF(rcl8, uint8_t, "rcl", "b")
DEF_ROT_CF(rcl16, uint16_t, "rcl", "w")
DEF_ROT_CF(rcl32, uint32_t, "rcl", "l")
DEF_ROT_CF(rcl64, uint64_t, "rcl", "q")
DEF_ROT_CF(rcr8, uint8_t, "rcr", "b")
DEF_ROT_CF(rcr16, uint16_t, "rcr", "w")
DEF_ROT_CF(rcr32, uint32_t, "rcr", "l")
DEF_ROT_CF(rcr64, uint64_t, "rcr", "q")

#define DEF_DSH(NAME, T, OP, SFX)                                             \
    static T NAME(T a, T b, uint8_t n) {                                      \
        __asm__(OP SFX " %%cl, %[b], %[a]" : [a] "+r"(a) : [b] "r"(b), "c"(n) \
                : "cc");                                                      \
        return a;                                                             \
    }
DEF_DSH(shld16, uint16_t, "shld", "w")
DEF_DSH(shld32, uint32_t, "shld", "l")
DEF_DSH(shld64, uint64_t, "shld", "q")
DEF_DSH(shrd16, uint16_t, "shrd", "w")
DEF_DSH(shrd32, uint32_t, "shrd", "l")
DEF_DSH(shrd64, uint64_t, "shrd", "q")

static uint64_t bsf64(uint64_t v, int *zf) {
    uint64_t r = 0;
    unsigned char z;
    __asm__("bsfq %[v], %[r]\n\tsetz %[z]" : [r] "+r"(r), [z] "=q"(z) : [v] "rm"(v) : "cc");
    *zf = z;
    return r;
}
static uint64_t bsr64(uint64_t v, int *zf) {
    uint64_t r = 0;
    unsigned char z;
    __asm__("bsrq %[v], %[r]\n\tsetz %[z]" : [r] "+r"(r), [z] "=q"(z) : [v] "rm"(v) : "cc");
    *zf = z;
    return r;
}
static uint32_t bsf32(uint32_t v, int *zf) {
    uint32_t r = 0;
    unsigned char z;
    __asm__("bsfl %[v], %[r]\n\tsetz %[z]" : [r] "+r"(r), [z] "=q"(z) : [v] "rm"(v) : "cc");
    *zf = z;
    return r;
}

static uint64_t bswap64(uint64_t v) {
    __asm__("bswapq %[v]" : [v] "+r"(v));
    return v;
}
static uint32_t bswap32(uint32_t v) {
    __asm__("bswapl %[v]" : [v] "+r"(v));
    return v;
}

static int bt64(uint64_t v, uint64_t b) {
    unsigned char c;
    __asm__("btq %[b], %[v]\n\tsetc %[c]" : [c] "=q"(c) : [v] "r"(v), [b] "r"(b) : "cc");
    return c;
}
static uint64_t btc64(uint64_t v, uint64_t b, int *cf) {
    unsigned char c;
    __asm__("btcq %[b], %[v]\n\tsetc %[c]" : [v] "+r"(v), [c] "=q"(c) : [b] "r"(b) : "cc");
    *cf = c;
    return v;
}
static uint64_t bts64(uint64_t v, uint64_t b, int *cf) {
    unsigned char c;
    __asm__("btsq %[b], %[v]\n\tsetc %[c]" : [v] "+r"(v), [c] "=q"(c) : [b] "r"(b) : "cc");
    *cf = c;
    return v;
}
static uint64_t btr64(uint64_t v, uint64_t b, int *cf) {
    unsigned char c;
    __asm__("btrq %[b], %[v]\n\tsetc %[c]" : [v] "+r"(v), [c] "=q"(c) : [b] "r"(b) : "cc");
    *cf = c;
    return v;
}
static uint32_t btc32(uint32_t v, uint32_t b, int *cf) {
    unsigned char c;
    __asm__("btcl %[b], %[v]\n\tsetc %[c]" : [v] "+r"(v), [c] "=q"(c) : [b] "r"(b) : "cc");
    *cf = c;
    return v;
}

static uint64_t neg64(uint64_t v, int *cf) {
    unsigned char c;
    __asm__("negq %[v]\n\tsetc %[c]" : [v] "+r"(v), [c] "=q"(c) : : "cc");
    *cf = c;
    return v;
}

/* 128-bit add/sub via adc/sbb. */
static void add128(uint64_t *lo, uint64_t *hi, uint64_t blo, uint64_t bhi) {
    uint64_t l = *lo, h = *hi;
    __asm__("addq %[blo], %[l]\n\tadcq %[bhi], %[h]"
            : [l] "+r"(l), [h] "+r"(h)
            : [blo] "r"(blo), [bhi] "r"(bhi)
            : "cc");
    *lo = l;
    *hi = h;
}
static void sub128(uint64_t *lo, uint64_t *hi, uint64_t blo, uint64_t bhi) {
    uint64_t l = *lo, h = *hi;
    __asm__("subq %[blo], %[l]\n\tsbbq %[bhi], %[h]"
            : [l] "+r"(l), [h] "+r"(h)
            : [blo] "r"(blo), [bhi] "r"(bhi)
            : "cc");
    *lo = l;
    *hi = h;
}

/* Full condition table after "cmpq b, a": writes 13 bytes to out[]. */
static void cmp_conds(uint64_t a, uint64_t b, uint8_t *out) {
    __asm__ volatile("cmpq %[b], %[a]\n\t"
                     "seto 0(%[p])\n\t"
                     "setc 1(%[p])\n\t"
                     "setz 2(%[p])\n\t"
                     "sets 3(%[p])\n\t"
                     "setp 4(%[p])\n\t"
                     "setl 5(%[p])\n\t"
                     "setg 6(%[p])\n\t"
                     "setb 7(%[p])\n\t"
                     "seta 8(%[p])\n\t"
                     "setle 9(%[p])\n\t"
                     "setge 10(%[p])\n\t"
                     "setbe 11(%[p])\n\t"
                     "setae 12(%[p])\n\t"
                     :
                     : [a] "r"(a), [b] "r"(b), [p] "r"(out)
                     : "cc", "memory");
}

/* lahf after "cmpq b, a"; returns AH. */
static uint8_t lahf_after_cmp(uint64_t a, uint64_t b) {
    uint8_t ah;
    __asm__ volatile("cmpq %[b], %[a]\n\tlahf\n\tmovb %%ah, %[o]"
                     : [o] "=q"(ah)
                     : [a] "r"(a), [b] "r"(b)
                     : "cc", "ax");
    return ah;
}

/* sahf with the given AH value; returns CF and ZF after it. */
static void sahf_load(uint8_t ah_val, unsigned char *cf, unsigned char *zf) {
    unsigned char c, z;
    __asm__ volatile("movb %[ah], %%ah\n\tsahf\n\tsetc %[c]\n\tsetz %[z]"
                     : [c] "=q"(c), [z] "=q"(z)
                     : [ah] "q"(ah_val)
                     : "cc", "ax");
    *cf = c;
    *zf = z;
}

static uint64_t cmov_lt(uint64_t a, uint64_t b, uint64_t x, uint64_t y) {
    uint64_t r = y;
    __asm__("cmpq %[b], %[a]\n\tcmovl %[x], %[r]"
            : [r] "+r"(r)
            : [a] "r"(a), [b] "r"(b), [x] "r"(x)
            : "cc");
    return r;
}

/* (remainder << 16) | quotient for 16-bit unsigned DX:AX / r16 with DX = 0. */
static uint32_t divw_asm(uint16_t ax, uint16_t d) {
    uint32_t q = ax;
    uint16_t rem;
    __asm__("xorl %%edx, %%edx\n\tdivw %[d]"
            : "+a"(q), "=&d"(rem)
            : [d] "rm"(d)
            : "cc");
    return (uint32_t)(q & 0xffff) | ((uint32_t)rem << 16);
}

/* AX / r8 (AL = quotient, AH = remainder); caller guarantees no #DE. */
static uint16_t divb_asm(uint16_t ax, uint8_t d) {
    uint16_t a = ax;
    __asm__("divb %[d]" : "+a"(a) : [d] "qm"(d) : "cc");
    return a;
}

static uint64_t cas64(volatile uint64_t *p, uint64_t *expect, uint64_t desired) {
    unsigned char ok;
    uint64_t e = *expect;
    __asm__ volatile("lock cmpxchgq %[d], %[m]\n\tsete %[ok]"
                     : [m] "+m"(*p), [ok] "=q"(ok), "+a"(e)
                     : [d] "r"(desired)
                     : "cc", "memory");
    *expect = e;
    return ok;
}
static uint32_t cas32(volatile uint32_t *p, uint32_t *expect, uint32_t desired) {
    unsigned char ok;
    uint32_t e = *expect;
    __asm__ volatile("lock cmpxchgl %[d], %[m]\n\tsete %[ok]"
                     : [m] "+m"(*p), [ok] "=q"(ok), "+a"(e)
                     : [d] "r"(desired)
                     : "cc", "memory");
    *expect = e;
    return ok;
}

typedef struct {
    uint64_t lo, hi;
} __attribute__((aligned(16))) u128_t;

static int cas128(u128_t *p, u128_t *expect, u128_t desired) {
    unsigned char ok;
    __asm__ volatile("lock cmpxchg16b %[m]\n\tsete %[ok]"
                     : [m] "+m"(*p), [ok] "=q"(ok), "+a"(expect->lo), "+d"(expect->hi)
                     : "b"(desired.lo), "c"(desired.hi)
                     : "cc", "memory");
    return ok;
}

#define DEF_XADD(NAME, T, SFX)                                                \
    static T NAME(volatile T *p, T v) {                                       \
        __asm__ volatile("lock xadd" SFX " %[v], %[m]"                        \
                         : [v] "+r"(v), [m] "+m"(*p)                          \
                         :                                                    \
                         : "cc", "memory");                                   \
        return v;                                                             \
    }
DEF_XADD(xadd8, uint8_t, "b")
DEF_XADD(xadd16, uint16_t, "w")
DEF_XADD(xadd32, uint32_t, "l")
DEF_XADD(xadd64, uint64_t, "q")

static uint64_t xchg64(volatile uint64_t *p, uint64_t v) {
    __asm__ volatile("xchgq %[v], %[m]" : [v] "+r"(v), [m] "+m"(*p) : : "memory");
    return v;
}

static unsigned popcnt_loop(uint64_t v) {
    unsigned c = 0;
    while (v) {
        v &= v - 1;
        c++;
    }
    return c;
}
static unsigned popcnt_scan(uint64_t v) {
    unsigned c = 0;
    for (int i = 0; i < 64; i++) c += (unsigned)((v >> i) & 1);
    return c;
}
static unsigned ctz_ref(uint64_t v) {
    unsigned i = 0;
    while (!(v & 1)) {
        v >>= 1;
        i++;
    }
    return i;
}
static unsigned clz_ref(uint64_t v) {
    unsigned i = 0;
    for (int b = 63; b >= 0 && !((v >> b) & 1); b--) i++;
    return i;
}

/* Portable references for the asm rotate/shift/carry helpers. */
static uint64_t ref_rol(uint64_t x, unsigned k, unsigned w) {
    uint64_t m = mask_w(w);
    x &= m;
    k %= w;
    if (!k) return x;
    return ((x << k) | (x >> (w - k))) & m;
}
static uint64_t ref_ror(uint64_t x, unsigned k, unsigned w) {
    return ref_rol(x, (w - k % w) % w, w);
}
static uint64_t ref_rcl(uint64_t x, unsigned k, unsigned w, int cf, int *cf_out) {
    unsigned __int128 c = ((unsigned __int128)(cf & 1) << w) | (x & mask_w(w));
    unsigned __int128 full = ((unsigned __int128)1 << (w + 1)) - 1;
    unsigned __int128 r = ((c << k) | (c >> (w + 1 - k))) & full;
    *cf_out = (int)((r >> w) & 1);
    return (uint64_t)(r & mask_w(w));
}
static uint64_t ref_rcr(uint64_t x, unsigned k, unsigned w, int cf, int *cf_out) {
    unsigned __int128 c = ((unsigned __int128)(cf & 1) << w) | (x & mask_w(w));
    unsigned __int128 full = ((unsigned __int128)1 << (w + 1)) - 1;
    unsigned __int128 r = ((c >> k) | (c << (w + 1 - k))) & full;
    *cf_out = (int)((r >> w) & 1);
    return (uint64_t)(r & mask_w(w));
}
static uint64_t ref_shld(uint64_t a, uint64_t b, unsigned k, unsigned w) {
    uint64_t m = mask_w(w);
    a &= m;
    b &= m;
    return ((a << k) | (b >> (w - k))) & m;
}
static uint64_t ref_shrd(uint64_t a, uint64_t b, unsigned k, unsigned w) {
    uint64_t m = mask_w(w);
    a &= m;
    b &= m;
    return ((a >> k) | (b << (w - k))) & m;
}

/* ---- sections ---------------------------------------------------------- */

static void sec_arith(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t r1 = rnd64(), r2 = rnd64();
        uint8_t ua = (uint8_t)r1, ub = (uint8_t)r2;
        uint16_t uc = (uint16_t)r1, ud = (uint16_t)r2;
        uint32_t ue = (uint32_t)r1, uf = (uint32_t)r2;
        uint64_t ug = r1, uh = r2;
        mix8(ua + ub); mix8(ua - ub); mix8(ua * ub); mix8(-ua);
        mix16(uc + ud); mix16(uc - ud); mix16(uc * ud); mix16(~uc);
        mix32(ue + uf); mix32(ue - uf); mix32(ue * uf); mix32(-ue);
        mix64(ug + uh); mix64(ug - uh); mix64(ug * uh); mix64(~ug);

        int8_t sa = (int8_t)ua, sb = (int8_t)ub, s8;
        mix8(__builtin_add_overflow(sa, sb, &s8)); mix8((uint8_t)s8);
        mix8(__builtin_sub_overflow(sa, sb, &s8)); mix8((uint8_t)s8);
        mix8(__builtin_mul_overflow(sa, sb, &s8)); mix8((uint8_t)s8);
        int16_t sc = (int16_t)uc, sd = (int16_t)ud, s16;
        mix8(__builtin_add_overflow(sc, sd, &s16)); mix16((uint16_t)s16);
        mix8(__builtin_sub_overflow(sc, sd, &s16)); mix16((uint16_t)s16);
        mix8(__builtin_mul_overflow(sc, sd, &s16)); mix16((uint16_t)s16);
        int32_t se = (int32_t)ue, sf = (int32_t)uf, s32;
        mix8(__builtin_add_overflow(se, sf, &s32)); mix32((uint32_t)s32);
        mix8(__builtin_sub_overflow(se, sf, &s32)); mix32((uint32_t)s32);
        mix8(__builtin_mul_overflow(se, sf, &s32)); mix32((uint32_t)s32);
        int64_t sg = (int64_t)ug, sh = (int64_t)uh, s64;
        mix8(__builtin_add_overflow(sg, sh, &s64)); mix64((uint64_t)s64);
        mix8(__builtin_sub_overflow(sg, sh, &s64)); mix64((uint64_t)s64);
        mix8(__builtin_mul_overflow(sg, sh, &s64)); mix64((uint64_t)s64);

        /* sign and zero extension / truncation */
        mix64((uint64_t)(int64_t)sa); mix64((uint64_t)(int64_t)sc);
        mix64((uint64_t)(int64_t)se); mix64((uint64_t)(uint8_t)sg);
        mix64((uint64_t)(uint16_t)sg); mix64((uint64_t)(uint32_t)sg);
    }
    end("arith");
}

static void fixed_arith(void) {
    volatile uint8_t a8 = 0xff;
    volatile uint16_t a16 = 0xffff;
    volatile uint32_t a32 = 0xffffffffu;
    volatile uint64_t a64 = ~0ULL;
    volatile int8_t m8 = -128;
    volatile int16_t m16 = -32768;
    volatile int32_t m32 = -2147483647 - 1;
    volatile int64_t m64 = -9223372036854775807LL - 1;
    printf("wrap: %u %u %u %llu\n", (unsigned)(uint8_t)(a8 + 1), (unsigned)(uint16_t)(a16 + 1),
           (unsigned)(a32 + 1u), (unsigned long long)(a64 + 1));
    printf("neg: %d %d %d %lld\n", (int)(int8_t)-m8, (int)(int16_t)-m16,
           (int)((uint32_t)0 - (uint32_t)m32 == (uint32_t)m32), (long long)(int64_t)((uint64_t)0 - (uint64_t)m64));
    int8_t r8;
    int16_t r16;
    int32_t r32;
    int64_t r64;
    printf("ovf: %d %d %d %d\n", __builtin_add_overflow(m8, (int8_t)-1, &r8),
           __builtin_sub_overflow(m16, (int16_t)1, &r16), __builtin_mul_overflow(m32, (int32_t)-1, &r32),
           __builtin_sub_overflow(m64, (int64_t)1, &r64));
}

static void fixed_div(void) {
    volatile int32_t a = -7, b = 2, c = 7, e = -2;
    printf("i32 div: %d r %d | %d r %d\n", a / b, a % b, c / e, c % e);
    volatile int64_t A = -7000000000LL, B = 3;
    printf("i64 div: %lld r %lld\n", (long long)(A / B), (long long)(A % B));
    volatile uint32_t U = 0xfffffff9u;
    printf("u32 div: %u r %u\n", U / 2u, U % 7u);
    volatile int8_t x8 = -128, y8 = -1;
    volatile int16_t x16 = -32768, y16 = 3;
    printf("i8 -128/-1=%d i16 -32768/3=%d r %d\n", (int)(int8_t)(x8 / y8), x16 / y16, x16 % y16);
}

static void sec_div(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t r = rnd64();
        int64_t n = (int64_t)r;
        uint64_t dv = rnd64();
        int64_t d = (int64_t)(dv >> (dv & 63));
        if (d == 0) d = 1;
        if (n == INT64_MIN && d == -1) d = -3;
        mix64((uint64_t)(n / d));
        mix64((uint64_t)(n % d));

        int32_t n32 = (int32_t)n, d32 = (int32_t)d;
        if (d32 == 0) d32 = 1;
        if (n32 == INT32_MIN && d32 == -1) d32 = -3;
        mix32((uint32_t)(n32 / d32));
        mix32((uint32_t)(n32 % d32));

        int16_t n16 = (int16_t)n, d16 = (int16_t)d;
        if (d16 == 0) d16 = 1;
        mix16((uint16_t)(n16 / d16));
        mix16((uint16_t)(n16 % d16));

        int8_t n8 = (int8_t)n, d8 = (int8_t)d;
        if (d8 == 0) d8 = 1;
        mix8((uint8_t)(n8 / d8));
        mix8((uint8_t)(n8 % d8));

        uint64_t un = (uint64_t)n, ud = (uint64_t)d;
        if (ud == 0) ud = 1;
        mix64(un / ud);
        mix64(un % ud);
        uint32_t un32 = (uint32_t)un, ud32 = (uint32_t)ud;
        if (ud32 == 0) ud32 = 1;
        mix32(un32 / ud32);
        mix32(un32 % ud32);
        uint16_t un16 = (uint16_t)un, ud16 = (uint16_t)ud;
        if (ud16 == 0) ud16 = 1;
        mix16(un16 / ud16);
        mix16(un16 % ud16);
        uint8_t un8 = (uint8_t)un, ud8 = (uint8_t)ud;
        if (ud8 == 0) ud8 = 1;
        mix8(un8 / ud8);
        mix8(un8 % ud8);

        /* asm 16-bit and 8-bit unsigned division, self-checked */
        uint16_t aw = (uint16_t)rnd64();
        uint16_t dw = (uint16_t)rnd64();
        if (dw == 0) dw = 3;
        uint32_t got = divw_asm(aw, dw);
        check((got & 0xffff) == (uint32_t)(aw / dw) && (got >> 16) == (uint32_t)(aw % dw));
        mix32(got);

        uint8_t db = (uint8_t)rnd64();
        if (db == 0) db = 7;
        uint16_t ab = (uint16_t)(rnd64() % (256u * db));
        uint16_t gotb = divb_asm(ab, db);
        check((gotb & 0xff) == ab / db && (gotb >> 8) == ab % db);
        mix16(gotb);
    }
    end("div");
}

static void sec_mul128(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t a = rnd64(), b = rnd64();
        unsigned __int128 p = (unsigned __int128)a * b;
        mix64((uint64_t)p);
        mix64((uint64_t)(p >> 64));

        __int128 sp = (__int128)(int64_t)a * (int64_t)b;
        mix64((uint64_t)sp);
        mix64((uint64_t)((unsigned __int128)sp >> 64));

        /* one-operand mul/imul produce rdx:rax */
        uint64_t lo = a, hi;
        __asm__("mulq %[b]" : "+a"(lo), "=&d"(hi) : [b] "r"(b) : "cc");
        check(lo == (uint64_t)p && hi == (uint64_t)(p >> 64));
        int64_t ilo = (int64_t)a, ihi;
        __asm__("imulq %[b]" : "+a"(ilo), "=&d"(ihi) : [b] "r"((int64_t)b) : "cc");
        check((uint64_t)ilo == (uint64_t)sp && (uint64_t)ihi == (uint64_t)((unsigned __int128)sp >> 64));

        /* 128 / 64 division through libgcc helpers */
        unsigned __int128 num = ((unsigned __int128)a << 64) | b;
        uint64_t den = rnd64() | 1;
        mix64((uint64_t)(num / den));
        mix64((uint64_t)(num % den));
        __int128 snum = (__int128)num;
        int64_t sden = (int64_t)(den >> 1);
        if (sden == 0) sden = 5;
        if (sden == -1) sden = -2;
        mix64((uint64_t)(snum / sden));
        mix64((uint64_t)(snum % sden));

        /* 64-bit signed idiv with cqto (self-checked) */
        int64_t qn = (int64_t)a, qd = (int64_t)(b >> 3);
        if (qd == 0) qd = -9;
        if (qn == INT64_MIN && qd == -1) qd = -7;
        int64_t q = qn, rem;
        __asm__("cqto\n\tidivq %[d]" : "+a"(q), "=&d"(rem) : [d] "r"(qd) : "cc");
        check(q == qn / qd && rem == qn % qd);
        mix64((uint64_t)q);
        mix64((uint64_t)rem);
    }
    end("mul128");
}

static void sec_shift(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t v = rnd64();
        unsigned n = (unsigned)(rnd64() & 63);
        mix64(v << n);
        mix64((uint64_t)((int64_t)v >> n));
        mix32((uint32_t)v << (n & 31));
        mix32((uint32_t)((int32_t)(uint32_t)v >> (n & 31)));
        mix16((uint16_t)v << (n & 15));
        mix16((uint16_t)((int16_t)(uint16_t)v >> (n & 15)));
        mix8((uint8_t)v << (n & 7));
        mix8((uint8_t)((int8_t)(uint8_t)v >> (n & 7)));

        /* rotates by cl; counts masked to 5 bits (8/16/32) or 6 bits (64) */
        unsigned k = (unsigned)(rnd64() & 31);
        uint8_t x8 = (uint8_t)v;
        uint16_t x16 = (uint16_t)v;
        uint32_t x32 = (uint32_t)v;
        uint64_t x64 = v;
        uint8_t r8 = rol8(x8, (uint8_t)k);
        check(r8 == (uint8_t)ref_rol(x8, k, 8));
        check(ror8(x8, (uint8_t)k) == (uint8_t)ref_ror(x8, k, 8));
        uint16_t r16 = rol16(x16, (uint8_t)k);
        check(r16 == (uint16_t)ref_rol(x16, k, 16));
        check(ror16(x16, (uint8_t)k) == (uint16_t)ref_ror(x16, k, 16));
        check(rol32(x32, (uint8_t)k) == (uint32_t)ref_rol(x32, k, 32));
        check(ror32(x32, (uint8_t)k) == (uint32_t)ref_ror(x32, k, 32));
        uint64_t k64 = rnd64() & 63;
        check(rol64(x64, (uint8_t)k64) == ref_rol(x64, (unsigned)k64, 64));
        check(ror64(x64, (uint8_t)k64) == ref_ror(x64, (unsigned)k64, 64));
        mix8(r8); mix16(r16); mix32(rol32(x32, (uint8_t)k)); mix64(ror64(x64, (uint8_t)k64));

        /* rcl/rcr: count 1..w-1, carry in random */
        int cf_in = (int)(rnd64() & 1);
        int cf_out, cf_ref;
        unsigned kc8 = 1 + (unsigned)(rnd64() % 7);
        uint8_t g8 = rcl8(x8, (uint8_t)kc8, cf_in, &cf_out);
        check(g8 == (uint8_t)ref_rcl(x8, kc8, 8, cf_in, &cf_ref) && cf_out == cf_ref);
        mix8(g8); mix8((uint8_t)cf_out);
        g8 = rcr8(x8, (uint8_t)kc8, cf_in, &cf_out);
        check(g8 == (uint8_t)ref_rcr(x8, kc8, 8, cf_in, &cf_ref) && cf_out == cf_ref);
        mix8(g8); mix8((uint8_t)cf_out);
        unsigned kc16 = 1 + (unsigned)(rnd64() % 15);
        uint16_t g16 = rcl16(x16, (uint8_t)kc16, cf_in, &cf_out);
        check(g16 == (uint16_t)ref_rcl(x16, kc16, 16, cf_in, &cf_ref) && cf_out == cf_ref);
        mix16(g16); mix8((uint8_t)cf_out);
        g16 = rcr16(x16, (uint8_t)kc16, cf_in, &cf_out);
        check(g16 == (uint16_t)ref_rcr(x16, kc16, 16, cf_in, &cf_ref) && cf_out == cf_ref);
        mix16(g16); mix8((uint8_t)cf_out);
        unsigned kc32 = 1 + (unsigned)(rnd64() % 31);
        uint32_t g32 = rcl32(x32, (uint8_t)kc32, cf_in, &cf_out);
        check(g32 == (uint32_t)ref_rcl(x32, kc32, 32, cf_in, &cf_ref) && cf_out == cf_ref);
        mix32(g32); mix8((uint8_t)cf_out);
        g32 = rcr32(x32, (uint8_t)kc32, cf_in, &cf_out);
        check(g32 == (uint32_t)ref_rcr(x32, kc32, 32, cf_in, &cf_ref) && cf_out == cf_ref);
        mix32(g32); mix8((uint8_t)cf_out);
        unsigned kc64 = 1 + (unsigned)(rnd64() % 63);
        uint64_t g64 = rcl64(x64, (uint8_t)kc64, cf_in, &cf_out);
        check(g64 == ref_rcl(x64, kc64, 64, cf_in, &cf_ref) && cf_out == cf_ref);
        mix64(g64); mix8((uint8_t)cf_out);
        g64 = rcr64(x64, (uint8_t)kc64, cf_in, &cf_out);
        check(g64 == ref_rcr(x64, kc64, 64, cf_in, &cf_ref) && cf_out == cf_ref);
        mix64(g64); mix8((uint8_t)cf_out);

        /* shld/shrd: count 1..w-1 */
        uint64_t y = rnd64();
        unsigned ks = 1 + (unsigned)(rnd64() % 15);
        uint16_t sl16 = shld16((uint16_t)v, (uint16_t)y, (uint8_t)ks);
        check(sl16 == (uint16_t)ref_shld((uint16_t)v, (uint16_t)y, ks, 16));
        uint16_t sr16 = shrd16((uint16_t)v, (uint16_t)y, (uint8_t)ks);
        check(sr16 == (uint16_t)ref_shrd((uint16_t)v, (uint16_t)y, ks, 16));
        mix16(sl16); mix16(sr16);
        ks = 1 + (unsigned)(rnd64() % 31);
        uint32_t sl32 = shld32((uint32_t)v, (uint32_t)y, (uint8_t)ks);
        check(sl32 == (uint32_t)ref_shld((uint32_t)v, (uint32_t)y, ks, 32));
        uint32_t sr32 = shrd32((uint32_t)v, (uint32_t)y, (uint8_t)ks);
        check(sr32 == (uint32_t)ref_shrd((uint32_t)v, (uint32_t)y, ks, 32));
        mix32(sl32); mix32(sr32);
        ks = 1 + (unsigned)(rnd64() % 63);
        uint64_t sl64 = shld64(v, y, (uint8_t)ks);
        check(sl64 == ref_shld(v, y, ks, 64));
        uint64_t sr64 = shrd64(v, y, (uint8_t)ks);
        check(sr64 == ref_shrd(v, y, ks, 64));
        mix64(sl64); mix64(sr64);
    }
    end("shift_rot");
}

static void sec_bits(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t v = rnd64();
        unsigned sh = (unsigned)(rnd64() & 63);
        if (v) {
            int zf;
            uint64_t f = bsf64(v, &zf);
            check(f == ctz_ref(v) && !zf);
            uint64_t r = bsr64(v, &zf);
            check(r == 63 - clz_ref(v) && !zf);
            mix64(f); mix64(r);
            uint32_t v32 = (uint32_t)v;
            if (v32) {
                uint32_t f32 = bsf32(v32, &zf);
                check(f32 == ctz_ref(v32) && !zf);
                mix32(f32);
            }
        } else {
            int zf;
            bsf64(0, &zf);
            check(zf == 1);
            bsr64(0, &zf);
            check(zf == 1);
            mix8(zf);
        }
        check(bswap64(v) == __builtin_bswap64(v));
        check(bswap32((uint32_t)v) == __builtin_bswap32((uint32_t)v));
        mix64(bswap64(v)); mix32(bswap32((uint32_t)v));

        int cf;
        int bit = bt64(v, sh);
        check(bit == (int)((v >> sh) & 1));
        mix8(bit);
        uint64_t t = btc64(v, sh, &cf);
        check(cf == (int)((v >> sh) & 1) && t == (v ^ (1ULL << sh)));
        mix64(t);
        t = bts64(v, sh, &cf);
        check(cf == (int)((v >> sh) & 1) && t == (v | (1ULL << sh)));
        mix64(t);
        t = btr64(v, sh, &cf);
        check(cf == (int)((v >> sh) & 1) && t == (v & ~(1ULL << sh)));
        mix64(t);
        check(btc32((uint32_t)v, sh & 31, &cf) == ((uint32_t)v ^ (1u << (sh & 31))));

        /* neg sets CF = (operand != 0) */
        uint64_t nv = (i & 7) == 0 ? 0 : v;
        uint64_t ng = neg64(nv, &cf);
        check(ng == (uint64_t)0 - nv && cf == (nv != 0));
        mix64(ng); mix8(cf);

        unsigned pc = popcnt_loop(v);
        check(pc == popcnt_scan(v));
        mix8(pc);
    }
    end("bits");
}

static void sec_flags(void) {
    begin();
    static const uint64_t edge[] = {0, 1, 2, ~0ULL, 0x8000000000000000ULL,
                                    0x7fffffffffffffffULL, 0x80, 0x7f, 0xffffffffULL};
    uint8_t out[16];
    for (int i = 0; i < 9 * 9 + 20000; i++) {
        uint64_t a, b;
        if (i < 81) {
            a = edge[i / 9];
            b = edge[i % 9];
        } else {
            a = rnd64();
            b = (i & 3) == 0 ? a : rnd64();
        }
        cmp_conds(a, b, out);
        for (int j = 0; j < 13; j++) mix8(out[j]);
        /* spot-check a few conditions against C semantics */
        check(out[5] == ((int64_t)a < (int64_t)b));
        check(out[6] == ((int64_t)a > (int64_t)b));
        check(out[7] == (a < b));
        check(out[8] == (a > b));
        check(out[2] == (a == b));
        check(out[12] == (a >= b));

        uint8_t ah = lahf_after_cmp(a, b);
        mix8(ah & 0xd5);
        check((ah & 1) == out[1] && ((ah >> 6) & 1) == out[2]);

        uint8_t sahv = (uint8_t)(rnd64() & 0xd5) | 0x02;
        unsigned char cfv, zfv;
        sahf_load(sahv, &cfv, &zfv);
        check(cfv == (sahv & 1) && zfv == ((sahv >> 6) & 1));
        mix8(cfv); mix8(zfv);

        uint64_t x = rnd64(), y = rnd64();
        mix64(cmov_lt(x, y, 111, 222));
        mix64(cmov_lt((int64_t)x, (int64_t)y, x, y));
    }
    end("flags");
}

static void sec_carry(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t alo = rnd64(), ahi = rnd64(), blo = rnd64(), bhi = rnd64();
        if (i & 1) bhi = ~0ULL;
        unsigned __int128 a = ((unsigned __int128)ahi << 64) | alo;
        unsigned __int128 b = ((unsigned __int128)bhi << 64) | blo;

        uint64_t lo = alo, hi = ahi;
        add128(&lo, &hi, blo, bhi);
        unsigned __int128 s = a + b;
        check(lo == (uint64_t)s && hi == (uint64_t)(s >> 64));
        mix64(lo); mix64(hi);

        lo = alo; hi = ahi;
        sub128(&lo, &hi, blo, bhi);
        unsigned __int128 d = a - b;
        check(lo == (uint64_t)d && hi == (uint64_t)(d >> 64));
        mix64(lo); mix64(hi);
    }
    end("carry");
}

static void sec_atomic(void) {
    begin();
    volatile uint64_t cell = 5;
    uint64_t exp = 5;
    int ok1 = (int)cas64(&cell, &exp, 9);
    uint64_t exp2 = 5;
    int ok2 = (int)cas64(&cell, &exp2, 11);
    check(ok1 == 1 && ok2 == 0 && cell == 9 && exp2 == 9);
    mix8(ok1); mix8(ok2); mix64(cell); mix64(exp2);

    volatile uint32_t c32 = 0xdeadbeef;
    uint32_t e32 = 0xdeadbeef;
    int ok3 = (int)cas32(&c32, &e32, 0x12345678u);
    check(ok3 == 1 && c32 == 0x12345678u);
    mix32(c32);

    u128_t cell128 = {5, 7};
    u128_t e128 = {5, 7};
    u128_t d128 = {9, 11};
    int ok4 = cas128(&cell128, &e128, d128);
    u128_t e128b = {5, 7};
    int ok5 = cas128(&cell128, &e128b, d128);
    check(ok4 == 1 && ok5 == 0 && cell128.lo == 9 && cell128.hi == 11 && e128b.lo == 9);
    mix64(cell128.lo); mix64(cell128.hi); mix8(ok4); mix8(ok5);

    for (int i = 0; i < 2000; i++) {
        uint64_t v = rnd64();
        volatile uint8_t p8 = (uint8_t)v;
        uint8_t old8 = xadd8(&p8, (uint8_t)(v >> 8));
        check(old8 == (uint8_t)v && p8 == (uint8_t)(v + (v >> 8)));
        mix8(old8); mix8(p8);

        volatile uint16_t p16 = (uint16_t)v;
        uint16_t old16 = xadd16(&p16, (uint16_t)(v >> 16));
        check(old16 == (uint16_t)v && p16 == (uint16_t)(v + (v >> 16)));
        mix16(old16); mix16(p16);

        volatile uint32_t p32 = (uint32_t)v;
        uint32_t old32 = xadd32(&p32, (uint32_t)(v >> 24));
        check(old32 == (uint32_t)v && p32 == (uint32_t)(v + (v >> 24)));
        mix32(old32); mix32(p32);

        volatile uint64_t p64 = v;
        uint64_t old64 = xadd64(&p64, v >> 40);
        check(old64 == v && p64 == v + (v >> 40));
        mix64(old64); mix64(p64);

        volatile uint64_t px = v;
        uint64_t oldx = xchg64(&px, ~v);
        check(oldx == v && px == ~v);
        mix64(oldx);
    }
    end("atomic");
}

static void sec_checksum(void) {
    begin();
    uint64_t acc = 0;
    for (uint32_t i = 0; i < 100000; i++) {
        uint64_t v = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
        acc += popcnt_loop(v) * 3 + ctz_ref(v | (1ULL << 40)) + clz_ref(v >> 3);
        acc ^= (v << (i & 63)) ^ (v >> ((i + 17) & 63));
        acc = (acc << 13) | (acc >> 51);
    }
    mix64(acc);
    end("checksum");
}

int main(void) {
    printf("t_int: integer conformance\n");
    fixed_arith();
    fixed_div();
    sec_arith();
    sec_div();
    sec_mul128();
    sec_shift();
    sec_bits();
    sec_flags();
    sec_carry();
    sec_atomic();
    sec_checksum();
    printf("t_int done\n");
    return 0;
}
