/*
 * libm.h -- what musl's math sources expect of musl's internal libm.h, for
 * building a few of them inside the toolkit on every host (src/math).
 *
 * Not musl's file: musl's src/internal/libm.h reaches for <endian.h>,
 * <features.h> and musl's own fp_arch.h, and spells its bit casts as
 * compound literals of anonymous unions. This keeps what the copied sources
 * use, in C that clang, GCC and MSVC all take, and adds the two things the
 * toolkit needs on top:
 *
 *   - every function and table gets a recomp_ name, so nothing here can bind
 *     to (or be replaced by) the host's libm or a compiler builtin, and
 *   - the arithmetic is the same on every host: no FMA (log2's
 *     __FP_FAST_FMA paths and table layout are forced off), and double_t
 *     must be double (FLT_EVAL_METHOD 0: SSE2 x86-64 and arm64 both).
 *
 * The functions are musl 1.2.5's generic C (src/math), see COPYRIGHT here.
 * recomp_math.h is the interface the rest of the toolkit uses.
 */
#ifndef RECOMP_MUSL_LIBM_H
#define RECOMP_MUSL_LIBM_H

#include <stdint.h>
#include <float.h>
#include <math.h>

#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#error "src/math needs FLT_EVAL_METHOD 0 (double_t == double): SSE2 or arm64, not x87"
#endif

/* log2.c and log2_data.[ch] fuse multiply-adds when the target has FMA
 * (arm64 does, the x86-64 baseline does not), which changes both the
 * arithmetic and the layout of the table. One path everywhere. */
#undef __FP_FAST_FMA

/* ---- names: the host's libm never sees these ---------------------------- */
#define sin                recomp_sin
#define cos                recomp_cos
#define tan                recomp_tan
#define atan               recomp_atan
#define atan2              recomp_atan2
#define log2               recomp_log2
#define exp2               recomp_exp2
#define __sin              recomp_m_ksin
#define __cos              recomp_m_kcos
#define __tan              recomp_m_ktan
#define __rem_pio2         recomp_m_rem_pio2
#define __rem_pio2_large   recomp_m_rem_pio2_large
#define __math_divzero     recomp_m_divzero
#define __math_invalid     recomp_m_invalid
#define __math_oflow       recomp_m_oflow
#define __math_uflow       recomp_m_uflow
#define __math_xflow       recomp_m_xflow
#define __log2_data        recomp_m_log2_data
#define __exp_data         recomp_m_exp_data

#if defined(__GNUC__) && !defined(_WIN32)
#define hidden __attribute__((__visibility__("hidden")))
#else
#define hidden
#endif

/* ---- musl's knobs, at musl's defaults ----------------------------------- */
#define WANT_ROUNDING 1
#define WANT_SNAN 0
#define issignalingf_inline(x) 0
#define issignaling_inline(x) 0
#define TOINT_INTRINSICS 0

#ifdef __GNUC__
#define predict_true(x)  __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x)  (x)
#define predict_false(x) (x)
#endif

static inline float  eval_as_float(float x)   { float y = x; return y; }
static inline double eval_as_double(double x) { double y = x; return y; }

static inline float  fp_barrierf(float x)  { volatile float y = x; return y; }
static inline double fp_barrier(double x)  { volatile double y = x; return y; }
static inline void   fp_force_evalf(float x)  { volatile float y; y = x; (void)y; }
static inline void   fp_force_eval(double x)  { volatile double y; y = x; (void)y; }
#define fp_barrierf    fp_barrierf
#define fp_barrier     fp_barrier
#define fp_force_evalf fp_force_evalf
#define fp_force_eval  fp_force_eval

/* Only float and double reach FORCE_EVAL in the copied sources. */
#define FORCE_EVAL(x) do {                        \
	if (sizeof(x) == sizeof(float)) {         \
		fp_force_evalf((float)(x));       \
	} else {                                  \
		fp_force_eval((double)(x));       \
	}                                         \
} while (0)

/* Bit casts, as functions rather than compound literals. */
static inline uint32_t recomp_m_asuint(float f)
{ union { float f; uint32_t i; } u; u.f = f; return u.i; }
static inline float recomp_m_asfloat(uint32_t i)
{ union { uint32_t i; float f; } u; u.i = i; return u.f; }
static inline uint64_t recomp_m_asuint64(double f)
{ union { double f; uint64_t i; } u; u.f = f; return u.i; }
static inline double recomp_m_asdouble(uint64_t i)
{ union { uint64_t i; double f; } u; u.i = i; return u.f; }
#define asuint(f)   recomp_m_asuint(f)
#define asfloat(i)  recomp_m_asfloat(i)
#define asuint64(f) recomp_m_asuint64(f)
#define asdouble(i) recomp_m_asdouble(i)

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64_t __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint32_t)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint32_t)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

/* ---- the internal functions the copied sources share --------------------- */
hidden int    __rem_pio2_large(double*,double*,int,int,int);
hidden int    __rem_pio2(double,double*);
hidden double __sin(double,double,int);
hidden double __cos(double,double);
hidden double __tan(double,double,int);
hidden double __math_xflow(uint32_t, double);
hidden double __math_uflow(uint32_t);
hidden double __math_oflow(uint32_t);
hidden double __math_divzero(uint32_t);
hidden double __math_invalid(double);

/* The public ones, under their recomp_ names (recomp_math.h has them too). */
double sin(double);
double cos(double);
double tan(double);
double atan(double);
double atan2(double, double);
double log2(double);
double exp2(double);

#endif
