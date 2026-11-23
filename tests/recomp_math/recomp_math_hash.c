/*
 * recomp_math_hash -- src/math gives the same bits on every host.
 *
 * Runs each recomp_ function over the same few million inputs (random bit
 * patterns, the ranges games use, multiples of pi/2, huge arguments for the
 * large reduction, the special values) and folds the results into one FNV-1a
 * hash per function. The hashes are a property of the code, not the host:
 * the expected ones are below, and a host that gives other bits fails.
 *
 * It also counts how often the host's own libm disagrees with recomp_ on
 * the same inputs (and by how many units in the last place), which is the
 * difference lifted code saw before src/math: run it on two systems and the
 * host columns differ while the recomp_ hashes do not.
 *
 *   recomp_math_hash            check against the expected hashes
 *   recomp_math_hash --print    print the hashes only (to fill them in)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "math/recomp_math.h"

static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double from_bits(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* NaN sign and payload are the host's (x86 SSE makes -NaN where arm64 makes
 * +NaN); the value is what is compared. */
static uint64_t canon(double d) { return d != d ? UINT64_C(0x7FF8000000000000) : bits(d); }

static uint64_t g_rng = UINT64_C(0x9E3779B97F4A7C15);
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * UINT64_C(2685821657736338717);
}
static double uniform(double lo, double hi) { return lo + (hi - lo) * ((double)(rnd() >> 11) / 9007199254740992.0); }

#define N_INPUTS 2600000
static double g_in[N_INPUTS], g_in2[N_INPUTS];
static int g_n;

static void add(double x, double y) { if (g_n < N_INPUTS) { g_in[g_n] = x; g_in2[g_n] = y; g_n++; } }

static void make_inputs(void)
{
    int i;
    g_rng = UINT64_C(0x9E3779B97F4A7C15);
    {
        const double s[] = { 0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0,
                             INFINITY, -INFINITY, NAN, 4.9406564584124654e-324,
                             2.2250738585072014e-308, 1.7976931348623157e308,
                             3.141592653589793, 1.5707963267948966, 0.7853981633974483,
                             1e-300, 1e300, 1e22, 1048576.0 * 3.141592653589793 };
        for (i = 0; i < (int)(sizeof s / sizeof s[0]); i++) {
            int j;
            for (j = 0; j < (int)(sizeof s / sizeof s[0]); j++)
                add(s[i], s[j]);
        }
    }
    /* One draw per statement: the order a compiler evaluates arguments in is
     * its own (GCC on x86-64 goes right to left, clang left to right), and
     * two draws in one call would give each compiler different inputs. */
    for (i = 0; i < 600000; i++) {                    /* every class of double */
        double a = from_bits(rnd());
        double b = from_bits(rnd());
        add(a, b);
    }
    for (i = 0; i < 1000000; i++) {                   /* what games pass */
        double a = uniform(-8.0 * 3.141592653589793, 8.0 * 3.141592653589793);
        double b = uniform(-64.0, 64.0);
        add(a, b);
    }
    for (i = 0; i < 300000; i++) {                    /* near multiples of pi/2 */
        double k = (double)(int64_t)(rnd() % 2000001) - 1000000.0;
        double off = uniform(-1e-9, 1e-9);
        double b = uniform(-1.0, 1.0);
        add(k * 1.5707963267948966 + off, b);
    }
    for (i = 0; i < 200000; i++) {                    /* the large reduction */
        double m = uniform(1.0, 2.0);
        int e = (int)(rnd() % 1000) + 20;
        double sign = (rnd() & 1) ? -1.0 : 1.0;
        double b = uniform(-1e6, 1e6);
        add(ldexp(m, e) * sign, b);
    }
    for (i = 0; i < 300000; i++) {                    /* exp2/log2 ranges */
        double a = uniform(-1100.0, 1100.0);
        double m = uniform(0.5, 1.0);
        int e = (int)(rnd() % 2100) - 1074;
        add(a, ldexp(m, e));
    }
}

typedef double (*fn1)(double);
typedef double (*fn2)(double, double);

struct row { const char *name; fn1 r1, h1; fn2 r2, h2; int second_arg_input; uint64_t expect; };

static double host_sin(double x) { return sin(x); }
static double host_cos(double x) { return cos(x); }
static double host_tan(double x) { return tan(x); }
static double host_atan(double x) { return atan(x); }
static double host_atan2(double y, double x) { return atan2(y, x); }
static double host_log2(double x) { return log2(x); }
static double host_exp2(double x) { return exp2(x); }

static uint64_t ulps(double a, double b)
{
    int64_t ia = (int64_t)bits(a), ib = (int64_t)bits(b);
    if (ia < 0) ia = INT64_MIN - ia;
    if (ib < 0) ib = INT64_MIN - ib;
    return ia > ib ? (uint64_t)(ia - ib) : (uint64_t)(ib - ia);
}

int main(int argc, char **argv)
{
    /* From Windows (zig 0.17), Linux (clang 21, GCC 15); every host must give these. */
    struct row rows[] = {
        { "sin",   recomp_sin,   host_sin,  NULL, NULL, 0, UINT64_C(0x7df086104366514b) },
        { "cos",   recomp_cos,   host_cos,  NULL, NULL, 0, UINT64_C(0xc15db83272277133) },
        { "tan",   recomp_tan,   host_tan,  NULL, NULL, 0, UINT64_C(0x4c32058133d4250f) },
        { "atan",  recomp_atan,  host_atan, NULL, NULL, 0, UINT64_C(0xaf16e3bfd91a2166) },
        { "atan2", NULL, NULL, recomp_atan2, host_atan2, 1, UINT64_C(0x186534ebdf8d34ec) },
        { "log2",  recomp_log2,  host_log2, NULL, NULL, 1, UINT64_C(0x0049b8c523365314) },
        { "exp2",  recomp_exp2,  host_exp2, NULL, NULL, 0, UINT64_C(0xd32af5c3ddb09f7c) },
    };
    int print_only = argc > 1 && strcmp(argv[1], "--print") == 0;
    int r, i, failed = 0;

    make_inputs();
    printf("recomp_math_hash: %d inputs\n", g_n);
    for (r = 0; r < (int)(sizeof rows / sizeof rows[0]); r++) {
        uint64_t h = UINT64_C(0xCBF29CE484222325), worst = 0;
        long differ = 0;
        for (i = 0; i < g_n; i++) {
            double x = rows[r].second_arg_input && rows[r].r1 ? g_in2[i] : g_in[i];
            double mine = rows[r].r2 ? rows[r].r2(g_in[i], g_in2[i]) : rows[r].r1(x);
            double host = rows[r].h2 ? rows[r].h2(g_in[i], g_in2[i]) : rows[r].h1(x);
            uint64_t c = canon(mine);
            int b;
            for (b = 0; b < 8; b++) {
                h ^= (c >> (8 * b)) & 0xFF;
                h *= UINT64_C(0x100000001B3);
            }
            if (canon(mine) != canon(host)) {
                differ++;
                if (mine == mine && host == host && ulps(mine, host) > worst)
                    worst = ulps(mine, host);
            }
        }
        printf("  %-5s recomp %016llx%s   host libm differs on %ld (max %llu ulp)\n",
               rows[r].name, (unsigned long long)h,
               print_only || !rows[r].expect ? "" : h == rows[r].expect ? "  ok" : "  MISMATCH",
               differ, (unsigned long long)worst);
        if (!print_only && rows[r].expect && h != rows[r].expect)
            failed = 1;
    }
    return failed;
}
