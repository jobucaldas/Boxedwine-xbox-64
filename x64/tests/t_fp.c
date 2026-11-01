/* t_fp.c - x86-64 floating point conformance: SSE float/double arithmetic
 * (bit-exact hashes), int<->float conversions, rounding modes and exception
 * flags (fenv), NaN/inf/denormal handling, x87 long double, strtod/strtold
 * and libm.
 *
 * Single-threaded and deterministic. Arithmetic is hashed on exact bit patterns.
 * libm results are printed with %.12g: glibc picks FMA-based implementations
 * through CPUID ifuncs, so last-bit differences between hosts are not a
 * conformance failure.
 *
 */
// LINK: -lm
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t hash;
static uint64_t rng = 0x2545F4914F6CDD1DULL;

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
static void mix32(uint64_t v) {
    for (int i = 0; i < 4; i++) mix8(v >> (i * 8));
}
static void mix64(uint64_t v) {
    for (int i = 0; i < 8; i++) mix8(v >> (i * 8));
}
static void mixd(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    mix64(u);
}
static void mixf(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    mix32(u);
}
static void mixld(long double x) {
    unsigned char b[16];
    memset(b, 0, sizeof b);
    memcpy(b, &x, 10); /* x87 extended: 80 significant bits, padding excluded */
    for (int i = 0; i < 10; i++) mix8(b[i]);
}

static void begin(void) { hash = FNV_OFFSET; }
static void end(const char *name) { printf("%-12s %016llx\n", name, (unsigned long long)hash); }

/* Random finite double with exponent in [-40, 39]. */
static double rnd_dbl(void) {
    uint64_t r = rnd64();
    double m = (double)(r >> 11) / 9007199254740992.0;
    int e = (int)(rnd64() % 80) - 40;
    double v = ldexp(1.0 + m, e);
    return (r & 1) ? -v : v;
}
static float rnd_flt(void) { return (float)rnd_dbl(); }

/* ---- sections ---------------------------------------------------------- */

static void sec_arith(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        volatile double a = rnd_dbl(), b = rnd_dbl();
        mixd(a + b);
        mixd(a - b);
        mixd(a * b);
        mixd(a / b);
        mixd(sqrt(fabs(a)));
        mixd(fmin(a, b));
        mixd(fmax(a, b));
        mixd(fma(a, b, 0.5)); /* libm fma, exact by definition */

        volatile float fa = rnd_flt(), fb = rnd_flt();
        mixf(fa + fb);
        mixf(fa - fb);
        mixf(fa * fb);
        mixf(fa / fb);
        mixf(sqrtf(fabsf(fa)));
        /* float <-> double round trips */
        mixd((double)fa * (double)fb);
        mixf((float)((double)fa / (double)fb));
    }
    end("arith");
}

static void sec_conv(void) {
    begin();
    for (int i = 0; i < 20000; i++) {
        uint64_t r = rnd64();
        volatile int64_t i64 = (int64_t)r;
        volatile int32_t i32 = (int32_t)(r >> 17);
        volatile uint64_t u64 = r;
        volatile uint32_t u32 = (uint32_t)(r >> 9);
        mixd((double)i64);
        mixf((float)i64);
        mixd((double)i32);
        mixf((float)i32);
        mixd((double)u64);
        mixf((float)u64);
        mixd((double)u32);
        mixf((float)u32);
        /* truncating double -> int for in-range values */
        /* keep values inside int32 range so the conversions are not UB in C */
        volatile double d = fmod(rnd_dbl() * 1000.0, 2e9);
        mix32((uint32_t)(int32_t)d);
        mix64((uint64_t)(int64_t)d);
        volatile float f = (float)fmod((double)rnd_flt() * 1000.0, 2e9);
        mix32((uint32_t)(int32_t)f);
        mix64((uint64_t)(int64_t)f);
        mixd((double)(float)d);
        mixd((double)f);
    }
    end("convert");
}

static void fixed_conv(void) {
    volatile double neg27 = -2.7, neg05 = -0.5, p25 = 2.5, p35 = 3.5, nm25 = -2.5;
    printf("trunc: %d %d %d\n", (int)neg27, (int)neg05, (int)(-0.99));
    printf("lrint: %ld %ld %ld %ld\n", lrint(p25), lrint(p35), lrint(nm25), lrint(-3.5));
    printf("round: %g %g %g %g\n", round(p25), round(nm25), round(-0.5), round(0.49999999999999994));
    printf("floor/ceil: %g %g %g %g\n", floor(neg27), ceil(neg27), trunc(neg27), rint(-0.5));
    volatile double big = 1e19, bigneg = -1e19, huge = 1e30;
    printf("u64: %llu\n", (unsigned long long)big);
    /* out-of-range cvttsd2si yields the "integer indefinite" value 0x8000... */
    printf("indef: %llx %llx %x\n", (unsigned long long)(int64_t)huge,
           (unsigned long long)(int64_t)bigneg, (unsigned)(int32_t)huge);
    volatile float fbig = 3e9f;
    printf("u32: %u\n", (unsigned)(uint32_t)(double)fbig);
    volatile int64_t i = 16777217; /* 2^24 + 1 is not representable in float */
    volatile int64_t j = 9007199254740993LL; /* 2^53 + 1 */
    printf("float round: %a %a\n", (double)(float)i, (double)j);
    printf("int->dbl: %.17g %.17g\n", (double)j, (double)(float)3.4e38f);
    volatile float tenth = 0.1f;
    printf("0.1f: %a %.17g\n", (double)tenth, (double)tenth);
}

static void fixed_special(void) {
    volatile double nan1 = NAN, one = 1.0, zero = 0.0, inf = INFINITY;
    double nz = -nan1; /* sign bit set */
    printf("nan: %g %g %a %a\n", nan1, nz, inf, -inf);
    printf("inf-inf=%a 0*inf=%a 0/0=%a\n", inf - inf, zero * inf, zero / zero);
    printf("nan cmp: %d %d %d %d %d %d\n", nan1 == one, nan1 != one, nan1 < one, nan1 > one,
           nan1 <= one, nan1 >= one);
    printf("nan eq self: %d %d unord=%d isnan=%d\n", nan1 == nan1, !(nan1 == nan1),
           isunordered(nan1, one) ? 1 : 0, isnan(nan1) ? 1 : 0);
    printf("fmin/fmax nan: %g %g %g %g\n", fmin(nan1, one), fmax(one, nan1), fmin(nan1, nan1), fmax(zero, nan1));
    printf("ternary min nan: %g %g\n", nan1 < one ? nan1 : one, one < nan1 ? nan1 : one);
    printf("sqrt(-1)=%a sqrt(-0)=%a sqrt(inf)=%a\n", sqrt(-one), sqrt(-zero), sqrt(inf));
    printf("neg zero: %a %d %d\n", -zero, signbit(-zero) ? 1 : 0, (-zero) == zero);
    volatile double denorm = 4.9406564584124654e-324; /* smallest subnormal */
    printf("denorm: %a %a %a\n", denorm, denorm * 0.5, denorm * 3.0);
    printf("max/min: %a %a %a\n", DBL_MAX, DBL_MIN, DBL_EPSILON);
    volatile float fnan = NAN, finf = INFINITY;
    printf("float special: %g %a %a %d\n", (double)fnan, (double)finf, (double)(finf - finf), isnan(fnan));
    printf("fmod/remainder: %g %g %g\n", fmod(10.5, 3.0), remainder(10.5, 3.0), fmod(-10.5, 3.0));
    printf("ldexp/frexp: %g", ldexp(1.5, 10));
    int ex;
    double mant = frexp(-48.0, &ex);
    printf(" %g %d\n", mant, ex);
    double ip;
    double frac = modf(-3.75, &ip);
    printf("modf: %g %g\n", ip, frac);
}

/* prints the exception flags of one case and folds them into the section hash */
static void flagline(const char *name, unsigned f) {
    printf("flags %-10s %x\n", name, f);
    mix32(f);
}

static void sec_fenv(void) {
    begin();
    volatile double one = 1.0, three = 3.0, ten16 = 1e16, tiny = 1.0;
    static const int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    for (int m = 0; m < 4; m++) {
        fesetround(modes[m]);
        double q = one / three;
        double s = ten16 + tiny;
        float qf = (float)one / (float)three;
        printf("round mode %d: %a %a %a\n", modes[m], q, s, (double)qf);
        mixd(q); mixd(s); mixf(qf);
        for (int i = 0; i < 2000; i++) {
            volatile double a = rnd_dbl(), b = rnd_dbl();
            mixd(a / b);
            mixd(a * b);
        }
    }
    fesetround(FE_TONEAREST);
    end("fenv_round");

    begin();
    volatile double zero = 0.0, big = 1e308, tinyv = 1e-300, nanv = NAN;
    const char *names[] = {"inexact", "1/3", "1/0", "0/0", "overflow", "underflow", "sqrt(-1)", "nan cmp"};
    feclearexcept(FE_ALL_EXCEPT);
    volatile double r = one / three;
    flagline(names[0], (unsigned)fetestexcept(FE_ALL_EXCEPT));
    feclearexcept(FE_ALL_EXCEPT);
    r = one / zero;
    flagline(names[2], (unsigned)fetestexcept(FE_ALL_EXCEPT));
    feclearexcept(FE_ALL_EXCEPT);
    r = zero / zero;
    flagline(names[3], (unsigned)fetestexcept(FE_ALL_EXCEPT));
    feclearexcept(FE_ALL_EXCEPT);
    r = big * 10.0;
    flagline(names[4], (unsigned)fetestexcept(FE_ALL_EXCEPT));
    feclearexcept(FE_ALL_EXCEPT);
    r = tinyv * tinyv;
    flagline(names[5], (unsigned)fetestexcept(FE_ALL_EXCEPT));
    feclearexcept(FE_ALL_EXCEPT);
    r = sqrt(-one);
    flagline(names[6], (unsigned)fetestexcept(FE_ALL_EXCEPT));
    feclearexcept(FE_ALL_EXCEPT);
    volatile int c = nanv < one;
    unsigned f7 = (unsigned)fetestexcept(FE_ALL_EXCEPT);
    flagline(names[7], f7);
    printf("   c=%d\n", c);
    mix8(c);
    feclearexcept(FE_ALL_EXCEPT);
    printf("r=%g\n", (double)r);
    end("fenv_flags");
}

static void sec_ld(void) {
    begin();
    volatile long double a = 1.0L, b = 3.0L;
    long double q = a / b;
    printf("ld: %.25Lf %La\n", q, q);
    printf("ld size=%d ldbl_max_exp=%d mant=%d\n", (int)sizeof(long double), LDBL_MAX_EXP, LDBL_MANT_DIG);
    volatile long double big = 1e4000L;
    printf("ld big: %Le %La\n", big * 0.5L, big * 1e-3L);
    printf("ld sqrt2: %.25Lf\n", sqrtl(2.0L));
    volatile long double u = 18446744073709551615.0L; /* 2^64 - 1 exact in x87 */
    printf("ld u64max: %.0Lf\n", u);
    volatile uint64_t uu = 0xFFFFFFFFFFFFFFFFULL;
    printf("ld from u64: %.0Lf\n", (long double)uu);
    for (int i = 0; i < 5000; i++) {
        volatile long double x = (long double)(int64_t)rnd64() / (long double)((rnd64() % 1000003) + 1);
        volatile long double y = (long double)rnd_dbl();
        mixld(x);
        mixld(x * y);
        mixld(x + y);
        mixld(x - y);
        mixld(y != 0 ? x / y : 0.0L);
        mixld(sqrtl(y < 0 ? -y : y));
        mix64((uint64_t)(int64_t)(x / 1024.0L));
        mixld((long double)(double)y);
        mixld((long double)(float)x);
    }
    for (int i = 0; i < 2000; i++) {
        volatile long double x = (long double)(i - 1000) * 0.37L;
        mixld(expl(x * 0.01L));
        mixld(logl(1.0L + (x < 0 ? -x : x)));
    }
    end("longdouble");
    printf("ld compare: %d %d\n", (int)(a < b), (int)(b == 3.0L));
}

static void sec_strtod(void) {
    static const char *inputs[] = {
        "0x1.8p3", "1e-310", "-0.0", "inf", "-INFINITY", "nan", "nan(0x123)", "1e400",
        "-1e-400", "  12.5e3xyz", "0x1p-1074", "4.9406564584124654e-324",
        "2.2250738585072011e-308", "1.7976931348623157e308", "0.1", "123456789012345678901234567890",
        "0x.8", "1e", "+.5e-2", ""};
    for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        char *end;
        errno = 0;
        double d = strtod(inputs[i], &end);
        int e1 = errno;
        printf("strtod %-30s -> %a %.17g errno=%d off=%ld\n", inputs[i], d, d, e1, (long)(end - inputs[i]));
    }
    for (unsigned i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        char *end;
        errno = 0;
        long double d = strtold(inputs[i], &end);
        int e1 = errno;
        printf("strtold %-29s -> %La errno=%d off=%ld\n", inputs[i], d, e1, (long)(end - inputs[i]));
    }
}

static void sec_libm(void) {
    volatile double x = 0.5, y = 1.0, z = 10.0, w = -2.0;
    printf("sin %.12g cos %.12g tan %.12g\n", sin(x), cos(x), tan(y));
    printf("asin %.12g acos %.12g atan %.12g atan2 %.12g\n", asin(x), acos(x), atan(y), atan2(y, -y));
    printf("exp %.12g exp(-20.5) %.12g\n", exp(y), exp(-20.5));
    printf("log %.12g log2 %.12g log10 %.12g log1p %.12g\n", log(z), log2(z), log10(z), log1p(0.25));
    printf("pow %.12g %.12g %.12g %.12g\n", pow(w, 3.0), pow(2.0, 0.5), pow(10.0, -3.0), pow(z, 15.5));
    printf("cbrt %.12g hypot %.12g expm1 %.12g\n", cbrt(27.0), hypot(3.0, 4.0), expm1(0.001));
    printf("sinh %.12g cosh %.12g tanh %.12g\n", sinh(x), cosh(x), tanh(y));
    printf("erf %.12g erfc %.12g\n", erf(x), erfc(y));
    printf("sin(1e10) %.12g cos(1e10) %.12g\n", sin(1e10), cos(1e10));
    printf("log(-1)=%g log(0)=%g exp(1000)=%g\n", log(-y), log(0.0 * y), exp(1000.0 * y));
    double acc = 0.0;
    for (int i = 1; i <= 2000; i++) {
        volatile double v = i * 0.001;
        acc += sin(v) * cos(v) + exp(-v) + log(1.0 + v) + sqrt(v);
    }
    printf("libm accumulate %.10g\n", acc);
}

int main(void) {
    printf("t_fp: floating point conformance\n");
    fixed_conv();
    fixed_special();
    sec_arith();
    sec_conv();
    sec_fenv();
    sec_ld();
    sec_strtod();
    sec_libm();
    printf("t_fp done\n");
    return 0;
}
