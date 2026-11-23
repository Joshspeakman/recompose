/*
 * recomp_math.h -- the x87 transcendentals, the same on every host.
 *
 * The lifter maps fsin, fcos, fsincos, fptan, fpatan, fyl2x, fyl2xp1 and
 * f2xm1 onto these (tools/recomp/lifter.py), not onto the host's libm.
 * glibc, Apple's libm and Windows' UCRT each round some results of sin, cos,
 * tan, atan2, log2 and exp2 differently in the last place. One copy of a game
 * never notices, but two copies on different systems playing each other do:
 * TimeSplitters 2 over System Link dropped every joiner whose host ran
 * another OS at the start of the match (Mac or Linux against Windows, either
 * way round), while Windows-Windows, Mac-Mac and Linux-Linux all held.
 *
 * These are musl 1.2.5's generic C implementations (src/math/musl, MIT),
 * renamed, built with contraction off, so the same inputs give the same bits
 * on x86-64 and arm64, with MSVC, clang, zig or GCC. sqrt, fmod, remainder,
 * ldexp, floor and the rest stay on the host: IEEE rounds them exactly, so
 * every libm already agrees.
 *
 * tests/recomp_math hashes each function over a fixed set of inputs; the
 * hashes are the same on every host by construction, and the test checks them.
 */
#ifndef RECOMP_MATH_H
#define RECOMP_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

double recomp_sin(double x);
double recomp_cos(double x);
double recomp_tan(double x);
double recomp_atan(double x);
double recomp_atan2(double y, double x);
double recomp_log2(double x);
double recomp_exp2(double x);

#ifdef __cplusplus
}
#endif

#endif
