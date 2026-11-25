/*
 * hle_d3d8_interp.c -- frame interpolation: frames shown between the
 * title's own, drawn by the toolkit.
 *
 * A title that steps its logic once a frame cannot run faster than 60
 * without running fast. This leaves the title at 60 and draws n-1 more
 * frames in each period (RECOMP_FRAME_INTERP=n, `frame_interp = n`): the
 * newest frame's draws again, with the matrices in their vertex constants
 * blended part of the way back toward the frame before.
 *
 * Where the time comes from. The flip gate (xbox_memory_layout.h) holds a
 * finished frame until its vblank -- TimeSplitters 2 finishes in about 5 ms
 * and waits about 11 -- and lends that wait here
 * (xbox_Nv2aFlipGateSetIdle). So frame N is complete before the moment
 * halfway between N-1 and N, and the in-between frame can be drawn from
 * N-1 and N with no added latency: N is still shown at its vblank, as it
 * always was. The newest frame is drawn into a second image
 * (xbox_D3D8InterpBegin), because the scene target still holds N.
 *
 * What is kept (hle_d3d8_record.c, hle_d3d8_interp_rec). Every host call
 * that changes state or draws, as an op of the deferred queue's kind, from
 * the frame's start; the frame starts with a snapshot of the state set
 * before it (hle_d3d8_interp_snapshot), so drawing the list again draws the
 * frame again. A draw is kept with its key -- a hash of its vertex and index
 * bytes, its program and shape, its textures and target -- and the values
 * of the registers this blends, as src/hle set them. Releases and program
 * deletes the list may still name wait until the list is done with.
 *
 * Matching (match_frames), frame N's draws to N-1's, in three tiers:
 *   1. the same bytes, program, shape and target. Of several (the same mesh
 *      drawn twice), the one nearest in translation;
 *   2. the same program, shape, textures and target: a mesh the CPU
 *      rewrites;
 *   3. the same program, stride, primitive type and target: a batch the CPU
 *      builds, whose count and texture change every frame (particles).
 *      Blended draws only: an opaque piece of the level matched this way
 *      took the place of the one the frame before drew there, which was
 *      then neither blended nor filled in (TS2, 24 Nov 2026: on a fast turn
 *      a slab of sky down the screen's edge the camera was leaving).
 * Unmatched draws are drawn as N drew them.
 *
 * Blending (the registers: xbox_D3D8SetInterpRegisters, interp_projection,
 * interp_affine). The projection is blended term by term, if both frames
 * have the same kind. Each 4-register affine matrix is split into rotation,
 * scale and translation; the rotation is blended as a quaternion and the
 * other two linearly, so a turning object keeps its size. A matrix that
 * turned more than 12 degrees or moved by half its distance in one frame
 * is a cut, and drawn as N has it in the world, moved back with the camera
 * (as is what has no partner). Only draws whose program goes through
 * the projection are blended: screen-space draws are N's exactly, so the
 * HUD does not shimmer.
 *
 * A frame is not interpolated (it is "held": the display keeps showing
 * N-1, as at 60) when it has no 3D, when less than 60% of its 3D
 * primitives matched, when more than half were cut, when a movie went
 * over it, while a capture is being written, or when the redraw would not
 * be done before the vblank.
 *
 * Limits: draws from push buffers the title fills itself never reach the
 * host, so cannot be redrawn. Particles the CPU positions stay at 60. A
 * title that does not clear the depth buffer each frame is not handled.
 * The deferred-frames mode (RECOMP_HLE_D3D8_DEFER) is not combined with
 * this; interpolation stays off there.
 *
 * Windows only, like the rest of shadow mode.
 */
#include "platform/xbox_winnt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "hle_d3d8_record.h"
#include "recomp_config.h"
#include "../kernel/xbox_memory_layout.h"
#include "recomp_lockstep.h"   /* recomp_lockstep_tick: RECOMP_INTERP_ACCOUNT */

#define INTERP_MAX_MULTIPLE  8
#define INTERP_MAX_AFFINE    64                  /* registers */
#define INTERP_MAX_REGS      (4 + INTERP_MAX_AFFINE)
#define INTERP_LIST_LIMIT    (96u * 1024u * 1024u)
#define INTERP_ALIGN(n)      (((n) + 15u) & ~(size_t)15u)

#define MIN_MATCHED          0.60                /* of the 3D primitives */
#define MAX_CUT              0.50
/* A frame's turn: cos(12 deg / 2). TS2 (Nov 2026): in-between frames lost
 * pieces of the level at the screen's edge the camera was leaving on fast
 * turns -- with a real mouse, 13% of them turning 15 to 20 degrees a frame
 * against 2 to 3% at 10 to 15, and walking at a wide field of view at 15;
 * at 30 and 60 far more. A flick faster than 12 degrees a frame (720 a
 * second at 60) is shown as the title's own frames. */
#define MAX_TURN_COS         0.99452190
#define MAX_MOVE             0.5                 /* of the distance */

typedef struct {
    uint32_t size;                               /* payload bytes */
    int32_t  draw;                               /* index into draws, or -1 */
    void (*fn)(const void *arg);
} op_header;

typedef struct {
    HleInterpDrawKey key;
    int match;                                   /* draw in the frame before, or -1 */
    int tier;
    int cut;                                     /* matched, but a matrix jumped */
    int reads_proj;                              /* its program reads the projection */
    uint32_t groups;                             /* the affine matrices it reads */
} InterpDraw;

typedef struct {
    uint8_t   *ops;
    size_t     len, cap;
    InterpDraw *draws;
    float     *regs;                             /* g_nregs float4s a draw */
    int        ndraws, cap_draws;
    uint8_t   *retire;                           /* ops run when the list is done */
    size_t     rlen, rcap;
    int        whole;                            /* began at a frame start, all kept */
} InterpList;

/* A matrix ready to blend, as the motion from the frame before to this one
 * (D = B A^-1, blended from the identity) and where it started (A): the
 * motion's rotation as a quaternion (the short way round), the rest of its
 * 3x3 and its translation. */
typedef struct {
    int    ok;
    double q[4];
    double s[9];
    double t[3];
    double a[12];                                /* A: 3 rows of 4 */
} Prepared;

static int        g_multiple;                    /* frames shown a frame drawn; <2 off */
static int        g_on;
static int        g_proj = -1, g_aff_first = -1, g_aff_count, g_nregs, g_groups;
static InterpList g_list[2];
static int        g_cur;                         /* the list being kept */
static Prepared  *g_prep;                        /* g_groups a draw of the current list */
static int        g_prep_cap;
static int        g_ref_draw = -1;               /* whose motion is the camera's */
/* The frame before's 3D draws with no partner in this one, to the target the
 * level is drawn to, and where in this list they go (fill_pass). */
static uint8_t   *g_orphan;
static int        g_orphan_cap, g_orphans;
static long long  g_inject_off = -1;
int               hle_d3d8_interp_replaying;

/* This frame, in the gate. */
static int        g_prepared, g_frame_ok, g_slot;
static LONGLONG   g_last_present, g_qpf, g_redraw_est, g_presented_at;

/* RECOMP_INTERP_DUMP=<prefix>: in-between frames from swap
 * RECOMP_INTERP_DUMP_FROM on, as <prefix>_i<swap>_<k>.bmp, the k-th of the
 * frames shown before that swap's frame; at most 24, as the frame dumps.
 * Beside RECOMP_HLE_D3D8_DUMP_EVERY=1 from the same swap, the frames either
 * side. */
static const char   *g_dump_prefix;
static unsigned long g_dump_from, g_swaps;
static int           g_dumped;

/* For the report. */
static unsigned long g_frames, g_shown, g_late, g_held_2d, g_held_match, g_held_cut,
                     g_held_other, g_presents_total;
static double        g_matched_sum, g_redraw_ms_sum, g_redraw_ms_max;
static double        g_alpha_sum, g_off_ms_sum, g_off_ms_max;
static unsigned long g_redraws, g_matched_frames;

unsigned long hle_d3d8_interp_presents(void)
{
    return g_presents_total;
}

/* ------------------------------------------------------------- the lists */

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
    size_t c = *cap ? *cap : (size_t)1 << 20;
    uint8_t *b;

    if (need <= *cap)
        return 1;
    while (c < need)
        c *= 2;
    b = (uint8_t *)realloc(*buf, c);
    if (!b)
        return 0;
    *buf = b;
    *cap = c;
    return 1;
}

static void push(uint8_t **buf, size_t *len, size_t *cap, void (*fn)(const void *),
                 const void *arg, size_t size, int draw, int *whole)
{
    size_t need = sizeof(op_header) + INTERP_ALIGN(size);
    op_header *h;

    if (*len + need > INTERP_LIST_LIMIT || !grow(buf, cap, *len + need)) {
        if (whole)
            *whole = 0;                          /* not the whole frame: not drawn again */
        return;
    }
    h = (op_header *)(*buf + *len);
    h->size = (uint32_t)size;
    h->draw = draw;
    h->fn = fn;
    if (size)
        memcpy(h + 1, arg, size);
    *len += need;
}

void hle_d3d8_interp_op(void (*fn)(const void *arg), const void *arg, size_t size)
{
    InterpList *l = &g_list[g_cur];

    push(&l->ops, &l->len, &l->cap, fn, arg, size, -1, &l->whole);
}

void hle_d3d8_interp_draw(void (*fn)(const void *arg), const void *arg, size_t size,
                          const HleInterpDrawKey *key)
{
    InterpList *l = &g_list[g_cur];
    const float *c = hle_d3d8_interp_constants();
    float *r;

    if (l->ndraws >= l->cap_draws) {
        int n = l->cap_draws ? l->cap_draws * 2 : 1024;
        InterpDraw *d = (InterpDraw *)realloc(l->draws, (size_t)n * sizeof *d);
        float *g;

        if (!d) {
            l->whole = 0;
            return;
        }
        l->draws = d;
        g = (float *)realloc(l->regs, (size_t)n * (size_t)(g_nregs ? g_nregs : 1) * 4 *
                             sizeof(float));
        if (!g) {
            l->whole = 0;
            return;
        }
        l->regs = g;
        l->cap_draws = n;
    }
    l->draws[l->ndraws].key = *key;
    l->draws[l->ndraws].match = -1;
    l->draws[l->ndraws].tier = 0;
    l->draws[l->ndraws].cut = 0;
    /* Only what the program reads is the draw's: a register it never reads
     * holds whatever was last put there, and may jump about. Asked now,
     * while the draw's program is the one bound. */
    l->draws[l->ndraws].reads_proj =
        key->uses_proj && g_proj >= 0 && (d3d8_vsh_bound_reads(g_proj, 4) & 1u);
    l->draws[l->ndraws].groups =
        key->uses_proj && g_aff_count ? d3d8_vsh_bound_reads(g_aff_first, g_aff_count) : 0;
    r = l->regs + (size_t)l->ndraws * (size_t)g_nregs * 4;
    if (g_proj >= 0) {
        memcpy(r, c + g_proj * 4, 16 * sizeof(float));
        r += 16;
    }
    if (g_aff_count)
        memcpy(r, c + g_aff_first * 4, (size_t)g_aff_count * 4 * sizeof(float));
    push(&l->ops, &l->len, &l->cap, fn, arg, size, l->ndraws, &l->whole);
    l->ndraws++;
}

/* Run what the list put off, with nothing kept while it runs: these are the
 * real releases now. */
static void list_retire(InterpList *l)
{
    int rec = hle_d3d8_interp_rec;
    size_t off;

    hle_d3d8_interp_rec = 0;
    for (off = 0; off < l->rlen; ) {
        op_header *h = (op_header *)(l->retire + off);
        h->fn(h->size ? (const void *)(h + 1) : NULL);
        off += sizeof(op_header) + INTERP_ALIGN(h->size);
    }
    hle_d3d8_interp_rec = rec;
    l->rlen = 0;
    l->len = 0;
    l->ndraws = 0;
    l->whole = 0;
}

void hle_d3d8_interp_retire(void (*fn)(const void *arg), const void *arg, size_t size)
{
    InterpList *l = &g_list[g_cur];
    size_t before = l->rlen;

    push(&l->retire, &l->rlen, &l->rcap, fn, arg, size, -1, NULL);
    if (l->rlen == before) {
        /* No room to put it off: do it now, and let the list go. */
        int rec = hle_d3d8_interp_rec;
        l->whole = 0;
        hle_d3d8_interp_rec = 0;
        fn(arg);
        hle_d3d8_interp_rec = rec;
    }
}

/* ------------------------------------------------------------- matching */

typedef uint64_t (*key_fn)(const HleInterpDrawKey *k);

static uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

static uint64_t key_same(const HleInterpDrawKey *k)
{
    uint64_t h = mix(k->content, k->vs);
    h = mix(h, ((uint64_t)k->stride << 32) | k->prims);
    h = mix(h, ((uint64_t)k->prim_type << 32) | k->index_format);
    return mix(h, (uint64_t)(uintptr_t)k->target);
}

static uint64_t key_shape(const HleInterpDrawKey *k)
{
    uint64_t h = mix(0x51ED270B27A3C1D5ull, k->vs);
    int s;

    h = mix(h, ((uint64_t)k->stride << 32) | k->prims);
    h = mix(h, ((uint64_t)k->prim_type << 32) | k->index_format);
    for (s = 0; s < 4; s++)
        h = mix(h, (uint64_t)(uintptr_t)k->tex[s]);
    return mix(h, (uint64_t)(uintptr_t)k->target);
}

static uint64_t key_batch(const HleInterpDrawKey *k)
{
    uint64_t h = mix(0x2545F4914F6CDD1Dull, k->vs);
    h = mix(h, ((uint64_t)k->stride << 32) | k->prim_type);
    return mix(h, (uint64_t)(uintptr_t)k->target);
}

/* The translation of a draw's first affine matrix, to choose between
 * copies of one mesh. */
static const float *draw_translation(const InterpList *l, int i, float out[3])
{
    const float *r;

    if (!g_aff_count)
        return NULL;
    r = l->regs + (size_t)i * (size_t)g_nregs * 4 + (g_proj >= 0 ? 16 : 0);
    out[0] = r[3];
    out[1] = r[7];
    out[2] = r[11];
    return out;
}

static void match_tier(InterpList *cur, const InterpList *prev, key_fn key, int tier,
                       uint8_t *used, uint64_t *keys, int *head, int *next, int size)
{
    int i, j;

    for (i = 0; i < size; i++)
        head[i] = -1;
    /* Chains in draw order: insert from the end. */
    for (j = prev->ndraws - 1; j >= 0; j--) {
        int slot;
        if (used[j] || (tier == 3 && !prev->draws[j].key.blended))
            continue;
        keys[j] = key(&prev->draws[j].key);
        slot = (int)(keys[j] & (uint64_t)(size - 1));
        next[j] = head[slot];
        head[slot] = j;
    }
    for (i = 0; i < cur->ndraws; i++) {
        InterpDraw *d = &cur->draws[i];
        uint64_t k;
        int best = -1, c;
        float ti[3], tc[3];
        double best_dist = 0.0;
        const float *t;

        if (d->match >= 0 || (tier == 3 && !d->key.blended))
            continue;
        k = key(&d->key);
        t = tier == 1 ? draw_translation(cur, i, ti) : NULL;
        for (c = head[(int)(k & (uint64_t)(size - 1))]; c >= 0; c = next[c]) {
            double dist;
            if (used[c] || keys[c] != k)
                continue;
            if (!t) {
                best = c;
                break;
            }
            /* Of several copies, the nearest: two identical crates must not
             * trade places halfway. */
            draw_translation(prev, c, tc);
            dist = (tc[0] - t[0]) * (tc[0] - t[0]) + (tc[1] - t[1]) * (tc[1] - t[1]) +
                   (tc[2] - t[2]) * (tc[2] - t[2]);
            if (best < 0 || dist < best_dist) {
                best = c;
                best_dist = dist;
            }
        }
        if (best >= 0) {
            used[best] = 1;
            d->match = best;
            d->tier = tier;
        }
    }
}

static void match_frames(InterpList *cur, const InterpList *prev)
{
    static uint8_t  *used;
    static uint64_t *keys;
    static int      *next, *head, cap, head_cap;
    int size = 1, i;

    while (size < prev->ndraws * 2)
        size <<= 1;
    if (prev->ndraws > cap) {
        cap = prev->ndraws * 2;
        used = (uint8_t *)realloc(used, (size_t)cap);
        keys = (uint64_t *)realloc(keys, (size_t)cap * sizeof *keys);
        next = (int *)realloc(next, (size_t)cap * sizeof *next);
    }
    if (size > head_cap) {
        head_cap = size;
        head = (int *)realloc(head, (size_t)head_cap * sizeof *head);
    }
    for (i = 0; i < cur->ndraws; i++) {
        cur->draws[i].match = -1;
        cur->draws[i].tier = 0;
        cur->draws[i].cut = 0;
    }
    if (!used || !keys || !next || !head || !prev->ndraws)
        return;
    memset(used, 0, (size_t)prev->ndraws);
    match_tier(cur, prev, key_same, 1, used, keys, head, next, size);
    match_tier(cur, prev, key_shape, 2, used, keys, head, next, size);
    match_tier(cur, prev, key_batch, 3, used, keys, head, next, size);
}

/* ------------------------------------------------------------- blending */

static double det3(const double *m)
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

/* The rotation nearest m (its polar factor), by Higham's iteration:
 * R <- (R + R^-T) / 2. A few steps for any matrix a title draws with. */
static int polar_rotation(const double *m, double *r)
{
    int k, i;

    memcpy(r, m, 9 * sizeof(double));
    for (k = 0; k < 24; k++) {
        double d = det3(r), cof[9], change = 0.0;

        if (fabs(d) < 1e-12)
            return 0;
        cof[0] =  (r[4] * r[8] - r[5] * r[7]);
        cof[1] = -(r[3] * r[8] - r[5] * r[6]);
        cof[2] =  (r[3] * r[7] - r[4] * r[6]);
        cof[3] = -(r[1] * r[8] - r[2] * r[7]);
        cof[4] =  (r[0] * r[8] - r[2] * r[6]);
        cof[5] = -(r[0] * r[7] - r[1] * r[6]);
        cof[6] =  (r[1] * r[5] - r[2] * r[4]);
        cof[7] = -(r[0] * r[5] - r[2] * r[3]);
        cof[8] =  (r[0] * r[4] - r[1] * r[3]);
        for (i = 0; i < 9; i++) {
            double n = 0.5 * (r[i] + cof[i] / d);  /* R^-T = cof(R) / det(R) */
            change += fabs(n - r[i]);
            r[i] = n;
        }
        if (change < 1e-9)
            break;
    }
    return 1;
}

static void quat_from(const double *r, double *q)
{
    double w = sqrt(fmax(0.0, 1.0 + r[0] + r[4] + r[8])) * 0.5;
    double x = sqrt(fmax(0.0, 1.0 + r[0] - r[4] - r[8])) * 0.5;
    double y = sqrt(fmax(0.0, 1.0 - r[0] + r[4] - r[8])) * 0.5;
    double z = sqrt(fmax(0.0, 1.0 - r[0] - r[4] + r[8])) * 0.5;
    double n;

    x = copysign(x, r[7] - r[5]);
    y = copysign(y, r[2] - r[6]);
    z = copysign(z, r[3] - r[1]);
    n = sqrt(w * w + x * x + y * y + z * z);
    q[0] = w / n;
    q[1] = x / n;
    q[2] = y / n;
    q[3] = z / n;
}

static void quat_to(const double *q, double *r)
{
    double w = q[0], x = q[1], y = q[2], z = q[3];

    r[0] = 1 - 2 * (y * y + z * z); r[1] = 2 * (x * y - z * w);     r[2] = 2 * (x * z + y * w);
    r[3] = 2 * (x * y + z * w);     r[4] = 1 - 2 * (x * x + z * z); r[5] = 2 * (y * z - x * w);
    r[6] = 2 * (x * z - y * w);     r[7] = 2 * (y * z + x * w);     r[8] = 1 - 2 * (x * x + y * y);
}

static int is_affine(const float *g)
{
    return fabsf(g[12]) < 1e-4f && fabsf(g[13]) < 1e-4f && fabsf(g[14]) < 1e-4f &&
           fabsf(g[15] - 1.0f) < 1e-4f;
}

static void mul3(const double *a, const double *b, double *out)
{
    int i, j, k;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            double v = 0.0;
            for (k = 0; k < 3; k++)
                v += a[i * 3 + k] * b[k * 3 + j];
            out[i * 3 + j] = v;
        }
}

/*
 * Blended by its motion. Each matrix is the camera's view of an object, so
 * a static object's motion from N-1 to N (D = B A^-1) is the camera's own,
 * the same for every piece of the level. Blending D from the identity and
 * applying it to A keeps the pieces together however fast the camera
 * turns; blending A and B themselves, rotation by quaternion and
 * translation linearly, pulled each piece toward the camera by its own
 * amount part way through a turn (cos of half the turn: 1% at 15 degrees a
 * frame), which opened cracks between neighbours.
 *
 * A cut (drawn as N has it): no inverse, a mirror flipping, a motion that
 * turns more than MAX_TURN, or one that moves more than MAX_MOVE of the
 * distance (an object teleported, a camera cut).
 */
static void prepare_group(Prepared *p, const float *a, const float *b, int *cut)
{
    double la[9], lb[9], ia[9], ld[9], r[9], d, da, db, dt;
    int i, j;

    p->ok = 0;
    if (memcmp(a, b, 16 * sizeof(float)) == 0)
        return;                                  /* nothing moved: drawn as it is */
    if (!is_affine(a) || !is_affine(b))
        return;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            la[i * 3 + j] = a[i * 4 + j];
            lb[i * 3 + j] = b[i * 4 + j];
        }
    d = det3(la);
    if (fabs(d) < 1e-12) {
        *cut = 1;
        return;
    }
    ia[0] =  (la[4] * la[8] - la[5] * la[7]) / d;
    ia[1] = -(la[1] * la[8] - la[2] * la[7]) / d;
    ia[2] =  (la[1] * la[5] - la[2] * la[4]) / d;
    ia[3] = -(la[3] * la[8] - la[5] * la[6]) / d;
    ia[4] =  (la[0] * la[8] - la[2] * la[6]) / d;
    ia[5] = -(la[0] * la[5] - la[2] * la[3]) / d;
    ia[6] =  (la[3] * la[7] - la[4] * la[6]) / d;
    ia[7] = -(la[0] * la[7] - la[1] * la[6]) / d;
    ia[8] =  (la[0] * la[4] - la[1] * la[3]) / d;
    mul3(lb, ia, ld);
    if (det3(ld) <= 0.0 || !polar_rotation(ld, r)) {
        *cut = 1;                                /* a mirror turning over */
        return;
    }
    /* S = R^T L_D, the rest */
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            p->s[i * 3 + j] = r[0 * 3 + i] * ld[0 * 3 + j] + r[1 * 3 + i] * ld[1 * 3 + j] +
                              r[2 * 3 + i] * ld[2 * 3 + j];
    quat_from(r, p->q);
    if (p->q[0] < 0.0)
        for (i = 0; i < 4; i++)
            p->q[i] = -p->q[i];
    for (i = 0; i < 3; i++)
        p->t[i] = b[i * 4 + 3] - (ld[i * 3 + 0] * a[3] + ld[i * 3 + 1] * a[7] +
                                  ld[i * 3 + 2] * a[11]);
    da = sqrt((double)a[3] * a[3] + (double)a[7] * a[7] + (double)a[11] * a[11]);
    db = sqrt((double)b[3] * b[3] + (double)b[7] * b[7] + (double)b[11] * b[11]);
    dt = sqrt(p->t[0] * p->t[0] + p->t[1] * p->t[1] + p->t[2] * p->t[2]);
    if (p->q[0] < MAX_TURN_COS || dt > MAX_MOVE * fmax(da, db) + 1e-3) {
        *cut = 1;                                /* a jump, not motion */
        return;
    }
    for (i = 0; i < 12; i++)
        p->a[i] = a[i];
    p->ok = 1;
}

/* The motion's rotation a of the way, slerped from the identity. */
static void motion_rotation(const Prepared *p, double a, double *r)
{
    double half = acos(fmin(1.0, p->q[0])), q[4], sn;
    int i;

    if (half < 1e-9) {
        quat_to(p->q, r);                        /* no turn to speak of */
        return;
    }
    sn = sin(half);
    q[0] = cos(a * half);
    for (i = 1; i < 4; i++)
        q[i] = p->q[i] / sn * sin(a * half);
    quat_to(q, r);
}

static void blend_group(const Prepared *p, double a, float *out)
{
    double r[9], s[9], l[9], la[9];
    int i, j;

    motion_rotation(p, a, r);
    for (i = 0; i < 9; i++)
        s[i] = (1.0 - a) * (i % 4 == 0 ? 1.0 : 0.0) + a * p->s[i];
    mul3(r, s, l);                               /* D's 3x3 a of the way */
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            la[i * 3 + j] = p->a[i * 4 + j];
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            out[i * 4 + j] = (float)(l[i * 3 + 0] * la[0 * 3 + j] + l[i * 3 + 1] * la[1 * 3 + j] +
                                     l[i * 3 + 2] * la[2 * 3 + j]);
        out[i * 4 + 3] = (float)(l[i * 3 + 0] * p->a[3] + l[i * 3 + 1] * p->a[7] +
                                 l[i * 3 + 2] * p->a[11] + a * p->t[i]);
    }
    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

static const float *regs_of(const InterpList *l, int i)
{
    return l->regs + (size_t)i * (size_t)g_nregs * 4;
}

/* Match frame N (the list kept) to N-1 and get every matched 3D draw's
 * matrices ready; decide whether the frame can be interpolated. */
static int prepare_frame(void)
{
    InterpList *cur = &g_list[g_cur], *prev = &g_list[g_cur ^ 1];
    double prims3d = 0.0, matched = 0.0, cut = 0.0;
    int i, g;

    if (!cur->ndraws) {
        g_held_2d++;                             /* nothing drawn: a load, a fade */
        return 0;
    }
    if (!cur->whole || !prev->whole) {
        if (getenv("RECOMP_INTERP_DEBUG"))
            fprintf(stderr, "[INTERP] swap %lu held: this frame %s, %d draws; the one "
                    "before %s, %d draws\n", g_swaps + 1, cur->whole ? "whole" : "not whole",
                    cur->ndraws, prev->whole ? "whole" : "not whole", prev->ndraws);
        g_held_other++;
        return 0;
    }
    if (cur->ndraws * g_groups > g_prep_cap) {
        int n = cur->ndraws * g_groups * 2;
        Prepared *p = (Prepared *)realloc(g_prep, (size_t)n * sizeof *p);
        if (!p) {
            g_held_other++;
            return 0;
        }
        g_prep = p;
        g_prep_cap = n;
    }
    match_frames(cur, prev);
    for (i = 0; i < cur->ndraws; i++) {
        InterpDraw *d = &cur->draws[i];

        if (!d->key.uses_proj)
            continue;
        prims3d += d->key.prims;
        if (d->match < 0)
            continue;
        matched += d->key.prims;
        {
            const float *a = regs_of(prev, d->match) + (g_proj >= 0 ? 16 : 0);
            const float *b = regs_of(cur, i) + (g_proj >= 0 ? 16 : 0);
            for (g = 0; g < g_groups; g++) {
                if ((d->groups >> g) & 1u)
                    prepare_group(&g_prep[i * g_groups + g], a + g * 16, b + g * 16, &d->cut);
                else
                    g_prep[i * g_groups + g].ok = 0;
            }
        }
        if (d->cut)
            cut += d->key.prims;
    }
    /* RECOMP_INTERP_ACCOUNT=<from>-<to>: for frames at those lockstep ticks,
     * the 3D primitives by what matching made of them, and how much of the
     * frame before found no partner in this one. */
    {
        static int ready;
        static unsigned long from = 1, to;
        if (!ready) {
            const char *v = getenv("RECOMP_INTERP_ACCOUNT");
            ready = 1;
            if (v && *v && sscanf(v, "%lu-%lu", &from, &to) != 2)
                to = from;
        }
        if (to && recomp_lockstep_tick() >= from && recomp_lockstep_tick() <= to) {
            double t[4] = {0, 0, 0, 0}, cutp = 0.0, orphan = 0.0, orphan_n = 0.0;
            int n_unm = 0, n_cut = 0, n_orph = 0;
            static uint8_t *partnered;
            static int pcap;
            if (prev->ndraws > pcap) {
                pcap = prev->ndraws * 2;
                partnered = (uint8_t *)realloc(partnered, (size_t)pcap);
            }
            if (partnered)
                memset(partnered, 0, (size_t)prev->ndraws);
            for (i = 0; i < cur->ndraws; i++) {
                const InterpDraw *d = &cur->draws[i];
                if (!d->key.uses_proj)
                    continue;
                if (d->match < 0) {
                    t[0] += d->key.prims;
                    n_unm++;
                    continue;
                }
                if (partnered)
                    partnered[d->match] = 1;
                t[d->tier] += d->key.prims;
                if (d->cut) {
                    cutp += d->key.prims;
                    n_cut++;
                }
            }
            for (i = 0; partnered && i < prev->ndraws; i++)
                if (prev->draws[i].key.uses_proj && !partnered[i]) {
                    orphan += prev->draws[i].key.prims;
                    orphan_n += 1.0;
                    n_orph++;
                }
            fprintf(stderr, "[INTERP-ACCT] tick %u: 3D %d draws %.0f prims; tier1 %.0f tier2 %.0f "
                    "tier3 %.0f; unmatched %.0f (%d draws); cut %.0f (%d draws); frame before "
                    "without a partner %.0f prims (%d draws)\n", recomp_lockstep_tick(),
                    cur->ndraws, prims3d, t[1], t[2], t[3], t[0], n_unm, cutp, n_cut, orphan,
                    n_orph);
        }
    }
    /* The camera's own motion, for the scissors and what is filled in
     * (redraw): the motion most of what was matched exactly shares, by
     * primitives -- the level's, since it holds still. A single piece could
     * be something moving on its own. */
    {
        static int *cand;
        static int cand_cap;
        int n = 0, a, b;
        double most = -1.0;

        g_ref_draw = -1;
        if (cur->ndraws > cand_cap) {
            int *c2 = (int *)realloc(cand, (size_t)cur->ndraws * 2 * sizeof *c2);
            if (c2) {
                cand = c2;
                cand_cap = cur->ndraws * 2;
            }
        }
        for (i = 0; g_groups && cand && i < cur->ndraws && i < cand_cap; i++) {
            const InterpDraw *d = &cur->draws[i];
            if (d->key.uses_proj && d->tier == 1 && !d->cut && (d->groups & 1u) &&
                g_prep[i * g_groups].ok)
                cand[n++] = i;
        }
        /* By primitives, each weighed down by how much nearer the eye than
         * most of the frame it is: what is carried in view space -- the
         * first-person weapon and hands, half a unit from TimeSplitters 2's
         * eye -- holds still on the screen while the camera turns, and on a
         * fast turn toward a bare wall its two pistols outnumbered the level
         * in view: the camera was taken to stand still, and what only the
         * frame before drew was left where it had been, a slab of sky down
         * the screen's edge. Agreeing is the same turn and the same move (a
         * sky that travels with the eye turns as the level does but does not
         * move as it does when the player walks). */
        static double *dist, *dsort;
        static int dist_cap;
        double med = 0.0;
        if (n > dist_cap) {
            double *d1 = (double *)realloc(dist, (size_t)n * 2 * sizeof *d1);
            double *d2 = (double *)realloc(dsort, (size_t)n * 2 * sizeof *d2);
            if (d1) dist = d1;
            if (d2) dsort = d2;
            if (d1 && d2) dist_cap = n * 2;
        }
        if (n > dist_cap)
            n = 0;
        for (a = 0; a < n; a++) {
            const float *m = regs_of(cur, cand[a]) + (g_proj >= 0 ? 16 : 0);
            dist[a] = sqrt((double)m[3] * m[3] + (double)m[7] * m[7] + (double)m[11] * m[11]);
            dsort[a] = dist[a];
        }
        for (a = 1; a < n; a++) {                /* a median: insertion sort, few pieces */
            double v = dsort[a];
            for (b = a - 1; b >= 0 && dsort[b] > v; b--)
                dsort[b + 1] = dsort[b];
            dsort[b + 1] = v;
        }
        if (n)
            med = dsort[n / 2];
        for (a = 0; a < n; a++) {
            const Prepared *pa = &g_prep[cand[a] * g_groups];
            double agree = 0.0;
            for (b = 0; b < n; b++) {
                const Prepared *pb = &g_prep[cand[b] * g_groups];
                double dot = pa->q[0] * pb->q[0] + pa->q[1] * pb->q[1] + pa->q[2] * pb->q[2] +
                             pa->q[3] * pb->q[3];
                double dx = pa->t[0] - pb->t[0], dy = pa->t[1] - pb->t[1], dz = pa->t[2] - pb->t[2];
                double w = cur->draws[cand[b]].key.prims;
                if (med > 0.0 && dist[b] < med)
                    w *= dist[b] / med;
                if (fabs(dot) > 0.99999 &&       /* within half a degree */
                    sqrt(dx * dx + dy * dy + dz * dz) <= 0.01 * med + 1e-6)
                    agree += w;
            }
            if (agree > most) {
                most = agree;
                g_ref_draw = cand[a];
            }
        }
    }
    /* RECOMP_INTERP_ACCOUNT also lists the matched pieces whose motion is not
     * the camera's (moving things -- or a piece paired with the wrong copy). */
    {
        const char *v = getenv("RECOMP_INTERP_ACCOUNT");
        unsigned long from = 0, to = 0;
        if (v && *v && sscanf(v, "%lu-%lu", &from, &to) != 2)
            to = from;
        if (to && g_ref_draw >= 0 && g_groups && recomp_lockstep_tick() >= from &&
            recomp_lockstep_tick() <= to) {
            const Prepared *r = &g_prep[g_ref_draw * g_groups];
            int odd = 0, said = 0;
            double odd_prims = 0.0;
            for (i = 0; i < cur->ndraws; i++) {
                const InterpDraw *d = &cur->draws[i];
                const Prepared *p = &g_prep[i * g_groups];
                const float *bm;
                double dot, dt, dist;
                int copies = 0, j;
                if (!d->key.uses_proj || d->match < 0 || d->cut || !(d->groups & 1u) || !p->ok)
                    continue;
                dot = fabs(p->q[0] * r->q[0] + p->q[1] * r->q[1] + p->q[2] * r->q[2] + p->q[3] * r->q[3]);
                dt = sqrt((p->t[0] - r->t[0]) * (p->t[0] - r->t[0]) + (p->t[1] - r->t[1]) * (p->t[1] - r->t[1]) +
                          (p->t[2] - r->t[2]) * (p->t[2] - r->t[2]));
                bm = regs_of(cur, i) + (g_proj >= 0 ? 16 : 0);
                dist = sqrt((double)bm[3] * bm[3] + (double)bm[7] * bm[7] + (double)bm[11] * bm[11]);
                if (dot > 0.99996 && dt < 0.02 * (dist + 1.0))
                    continue;
                odd++;
                odd_prims += d->key.prims;
                for (j = 0; j < cur->ndraws; j++)
                    if (key_same(&cur->draws[j].key) == key_same(&d->key))
                        copies++;
                if (copies > 1 && said++ < 24)
                    fprintf(stderr, "[INTERP-ACCT]   draw %d tier %d prims %u copies %d: turn off the "
                            "camera's by %.2f deg, moved off it by %.3f at distance %.2f\n", i, d->tier,
                            d->key.prims, copies, 2.0 * acos(dot > 1.0 ? 1.0 : dot) * 57.29578, dt, dist);
            }
            {
                const float *rb = regs_of(cur, g_ref_draw) + (g_proj >= 0 ? 16 : 0);
                fprintf(stderr, "[INTERP-ACCT] tick %u: %d matched pieces (%.0f prims) moved unlike the "
                        "camera; the camera's piece: draw %d, %u prims, turns %.2f deg, moves %.3f, "
                        "at distance %.2f\n", recomp_lockstep_tick(), odd, odd_prims, g_ref_draw,
                        cur->draws[g_ref_draw].key.prims,
                        2.0 * acos(fabs(r->q[0]) > 1.0 ? 1.0 : fabs(r->q[0])) * 57.29578,
                        sqrt(r->t[0] * r->t[0] + r->t[1] * r->t[1] + r->t[2] * r->t[2]),
                        sqrt((double)rb[3] * rb[3] + (double)rb[7] * rb[7] + (double)rb[11] * rb[11]));
            }
        }
    }
    /* What the frame before drew of the level and this one left out. The
     * game culls to what its newer camera sees, so on a turn an in-between
     * frame, looking part of the way back, shows places only the older
     * camera saw: through a doorway at the edge of the screen, whole rooms
     * missing, and the sky behind them (24 Nov 2026, TimeSplitters 2: 33
     * pieces of the frame before had no partner at 15 degrees a frame).
     * They are drawn too (fill_pass), moved as the camera moved; for a turn
     * the two frames' views take in every view in between. */
    g_orphans = 0;
    g_inject_off = -1;
    if (g_ref_draw >= 0 && prev->ndraws > 0) {
        const void *target = cur->draws[g_ref_draw].key.target;
        size_t off;

        if (prev->ndraws > g_orphan_cap) {
            uint8_t *o = (uint8_t *)realloc(g_orphan, (size_t)prev->ndraws * 2);
            if (o) {
                g_orphan = o;
                g_orphan_cap = prev->ndraws * 2;
            }
        }
        if (g_orphan && prev->ndraws <= g_orphan_cap) {
            for (i = 0; i < prev->ndraws; i++)
                g_orphan[i] = prev->draws[i].key.uses_proj &&
                              prev->draws[i].key.target == target;
            for (i = 0; i < cur->ndraws; i++)
                if (cur->draws[i].match >= 0)
                    g_orphan[cur->draws[i].match] = 0;
            for (i = 0; i < prev->ndraws; i++)
                g_orphans += g_orphan[i];
            /* After this list's last 3D draw to that target: before its
             * full-screen passes and its 2D, so those go over the filled
             * places as over the rest. */
            for (off = 0; off < cur->len; ) {
                const op_header *h = (const op_header *)(cur->ops + off);
                if (h->draw >= 0 && cur->draws[h->draw].key.uses_proj &&
                    cur->draws[h->draw].key.target == target)
                    g_inject_off = (long long)off;
                off += sizeof(op_header) + INTERP_ALIGN(h->size);
            }
        }
    }
    if (prims3d <= 0.0) {
        g_held_2d++;
        return 0;
    }
    g_matched_sum += matched / prims3d;
    g_matched_frames++;
    if (matched / prims3d < MIN_MATCHED) {
        g_held_match++;
        return 0;
    }
    /* How much is a cut, weighed as the camera's vote is: what is carried in
     * view space holds still while the camera turns past the limit, and with
     * a bare wall in view TS2's pistols were more than half the primitives,
     * so a flick cut the whole level and the frame was still interpolated --
     * the camera taken to stand still, a slab of sky at the screen's edge. */
    {
        static double *wd, *ws;
        static int wcap;
        double medw = 0.0, wcut = 0.0, wall = 0.0;
        int nn = 0, a2, b2;
        if (cur->ndraws > wcap) {
            double *n1 = (double *)realloc(wd, (size_t)cur->ndraws * 2 * sizeof *n1);
            double *n2 = (double *)realloc(ws, (size_t)cur->ndraws * 2 * sizeof *n2);
            if (n1) wd = n1;
            if (n2) ws = n2;
            if (n1 && n2) wcap = cur->ndraws * 2;
        }
        for (i = 0; i < cur->ndraws && i < wcap; i++) {
            const InterpDraw *d = &cur->draws[i];
            const float *m;
            wd[i] = -1.0;
            if (!d->key.uses_proj || !g_groups || !(d->groups & 1u))
                continue;
            m = regs_of(cur, i) + (g_proj >= 0 ? 16 : 0);
            wd[i] = sqrt((double)m[3] * m[3] + (double)m[7] * m[7] + (double)m[11] * m[11]);
            ws[nn++] = wd[i];
        }
        for (a2 = 1; a2 < nn; a2++) {
            double v = ws[a2];
            for (b2 = a2 - 1; b2 >= 0 && ws[b2] > v; b2--)
                ws[b2 + 1] = ws[b2];
            ws[b2 + 1] = v;
        }
        if (nn)
            medw = ws[nn / 2];
        for (i = 0; nn && i < cur->ndraws && i < wcap; i++) {
            const InterpDraw *d = &cur->draws[i];
            double w;
            if (wd[i] < 0.0)
                continue;
            w = d->key.prims * (medw > 0.0 && wd[i] < medw ? wd[i] / medw : 1.0);
            wall += w;
            if (d->cut)
                wcut += w;
        }
        if (wall > 0.0 && wcut / wall > MAX_CUT)
            cut = prims3d;                       /* held as a cut, below */
    }
    if (cut / prims3d > MAX_CUT) {
        static int debug = -1, said;
        if (debug < 0)
            debug = getenv("RECOMP_INTERP_DEBUG") != NULL;
        if (debug && said < 40) {
            fprintf(stderr, "[INTERP] swap %lu held as a cut: %.0f of %.0f 3D primitives\n",
                    g_swaps + 1, cut, prims3d);
            for (i = 0; i < cur->ndraws && said < 40; i++) {
                const InterpDraw *d = &cur->draws[i];
                if (d->cut) {
                    const float *a = regs_of(prev, d->match) + (g_proj >= 0 ? 16 : 0);
                    const float *b = regs_of(cur, i) + (g_proj >= 0 ? 16 : 0);
                    fprintf(stderr, "[INTERP]   draw %d tier %d vs %lx prims %u: "
                            "t (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n", i, d->tier,
                            (unsigned long)d->key.vs, d->key.prims, a[3], a[7], a[11],
                            b[3], b[7], b[11]);
                    said++;
                }
            }
        }
        g_held_cut++;
        return 0;
    }
    return 1;
}

/* The camera's motion from N back to the in-between frame being drawn,
 * D(a) D^-1 of the reference piece (3 rows of 4), for N's 3D draws with no
 * partner in the frame before: what N alone sees -- a room coming into view
 * on a turn -- moves with the rest instead of staying where N had it. */
static double g_cam_back[12];
static int    g_cam_back_ok;

static void camera_back(const Prepared *p, double a)
{
    double r[9], s[9], la[9], ld[9], il[9], x[9], d;
    int i;

    motion_rotation(p, a, r);
    for (i = 0; i < 9; i++)
        s[i] = (1.0 - a) * (i % 4 == 0 ? 1.0 : 0.0) + a * p->s[i];
    mul3(r, s, la);                              /* D(a)'s 3x3 */
    quat_to(p->q, r);
    mul3(r, p->s, ld);                           /* D's 3x3 */
    d = det3(ld);
    g_cam_back_ok = fabs(d) > 1e-12;
    if (!g_cam_back_ok)
        return;
    il[0] =  (ld[4] * ld[8] - ld[5] * ld[7]) / d;
    il[1] = -(ld[1] * ld[8] - ld[2] * ld[7]) / d;
    il[2] =  (ld[1] * ld[5] - ld[2] * ld[4]) / d;
    il[3] = -(ld[3] * ld[8] - ld[5] * ld[6]) / d;
    il[4] =  (ld[0] * ld[8] - ld[2] * ld[6]) / d;
    il[5] = -(ld[0] * ld[5] - ld[2] * ld[3]) / d;
    il[6] =  (ld[3] * ld[7] - ld[4] * ld[6]) / d;
    il[7] = -(ld[0] * ld[7] - ld[1] * ld[6]) / d;
    il[8] =  (ld[0] * ld[4] - ld[1] * ld[3]) / d;
    mul3(la, il, x);                             /* D(a) D^-1 */
    for (i = 0; i < 3; i++) {
        g_cam_back[i * 4 + 0] = x[i * 3 + 0];
        g_cam_back[i * 4 + 1] = x[i * 3 + 1];
        g_cam_back[i * 4 + 2] = x[i * 3 + 2];
        g_cam_back[i * 4 + 3] = a * p->t[i] - (x[i * 3 + 0] * p->t[0] + x[i * 3 + 1] * p->t[1] +
                                               x[i * 3 + 2] * p->t[2]);
    }
}

/* Draw i's registers at a: blended where it was matched and is 3D, N's
 * own otherwise -- set either way, since the draw before may have left
 * blended ones. */
static void set_draw_registers(int i, double a)
{
    const InterpList *cur = &g_list[g_cur], *prev = &g_list[g_cur ^ 1];
    const InterpDraw *d = &cur->draws[i];
    const float *b = regs_of(cur, i);
    float out[INTERP_MAX_REGS * 4];
    int blend = d->key.uses_proj && d->match >= 0 && !d->cut;
    int g, k;

    memcpy(out, b, (size_t)g_nregs * 4 * sizeof(float));
    /* What the frame before has no partner for, or whose motion was a jump
     * (a cut), is moved back with the camera: as N has it in the world, seen
     * from this frame's camera. Drawn as N has it on the screen instead, a
     * near wall cut on a fast turn was still off the screen where the
     * in-between frame had it in view, and the sky showed there. */
    if (!blend && g_cam_back_ok && d->key.uses_proj && (d->match < 0 || d->cut) &&
        g_ref_draw >= 0 && d->key.target == cur->draws[g_ref_draw].key.target) {
        float *o = out + (g_proj >= 0 ? 16 : 0);
        for (g = 0; g < g_groups; g++) {
            const float *m = b + (g_proj >= 0 ? 16 : 0) + g * 16;
            float *w = o + g * 16;
            int r;
            if (!((d->groups >> g) & 1u) || !is_affine(m))
                continue;
            for (r = 0; r < 3; r++) {
                for (k = 0; k < 4; k++)
                    w[r * 4 + k] = (float)(g_cam_back[r * 4 + 0] * m[0 * 4 + k] +
                                           g_cam_back[r * 4 + 1] * m[1 * 4 + k] +
                                           g_cam_back[r * 4 + 2] * m[2 * 4 + k] +
                                           (k == 3 ? g_cam_back[r * 4 + 3] : 0.0));
            }
        }
    }
    if (blend) {
        const float *pa = regs_of(prev, d->match);
        float *o = out;

        if (g_proj >= 0) {
            /* The same kind of projection both sides (the last row says
             * perspective or not), or it is a change of view, not motion. */
            if (d->reads_proj && memcmp(pa + 12, b + 12, 4 * sizeof(float)) == 0)
                for (k = 0; k < 16; k++)
                    o[k] = (float)((1.0 - a) * pa[k] + a * b[k]);
            o += 16;
        }
        for (g = 0; g < g_groups; g++) {
            const Prepared *p = &g_prep[i * g_groups + g];
            if (p->ok)
                blend_group(p, a, o + g * 16);
        }
    }
    if (g_proj >= 0) {
        d3d8_vsh_set_constant(g_proj, out, 4);
        if (g_aff_count)
            d3d8_vsh_set_constant(g_aff_first, out + 16, g_aff_count);
    } else if (g_aff_count) {
        d3d8_vsh_set_constant(g_aff_first, out, g_aff_count);
    }
}

/* ---------------------------------------------------------- drawing it */

/* The camera's motion from the frame before to this in-between frame (the
 * reference piece's, a of the way), applied to a draw of the frame before. */
static void camera_motion_apply(const Prepared *ref, double a, const float *g, float *out)
{
    Prepared p = *ref;
    int i;

    for (i = 0; i < 12; i++)
        p.a[i] = g[i];
    blend_group(&p, a, out);
}

/* The frame before's draws with no partner here (prepare_frame), drawn into
 * this in-between frame where they would be now: its whole list run again
 * for its state -- nothing cleared, copied or retargeted, and of its draws
 * only those -- then this list's state put back as it was at the point they
 * went in. Each is depth-tested against this frame's, so it shows only where
 * this frame drew nothing nearer. turn_n: the scissor turn for this list. */
/* RECOMP_INTERP_FILL=0: leave them out (for comparing). */
static int fill_off(void)
{
    static int off = -1;
    if (off < 0) {
        const char *v = getenv("RECOMP_INTERP_FILL");
        off = v && strcmp(v, "0") == 0;
    }
    return off;
}

static void fill_pass(double a, size_t upto, const double *turn_n, double sx, double sy)
{
    InterpList *cur = &g_list[g_cur], *prev = &g_list[g_cur ^ 1];
    const Prepared *ref = &g_prep[g_ref_draw * g_groups];
    double ra[9];
    size_t off;

    motion_rotation(ref, a, ra);                 /* the older camera to this frame */
    d3d8_SetScissorWarp(ra, sx, sy);
    for (off = 0; off < prev->len; ) {
        op_header *h = (op_header *)(prev->ops + off);
        const void *arg = h->size ? (const void *)(h + 1) : NULL;

        if (h->draw >= 0) {
            if (h->draw < g_orphan_cap && g_orphan[h->draw]) {
                const float *b = regs_of(prev, h->draw);
                float out[INTERP_MAX_REGS * 4];
                const InterpDraw *d = &prev->draws[h->draw];
                int g;

                memcpy(out, b, (size_t)g_nregs * 4 * sizeof(float));
                for (g = 0; g < g_groups; g++)
                    if (((d->groups >> g) & 1u) && is_affine(b + (g_proj >= 0 ? 16 : 0) + g * 16))
                        camera_motion_apply(ref, a, b + (g_proj >= 0 ? 16 : 0) + g * 16,
                                            out + (g_proj >= 0 ? 16 : 0) + g * 16);
                if (g_proj >= 0) {
                    d3d8_vsh_set_constant(g_proj, out, 4);
                    if (g_aff_count)
                        d3d8_vsh_set_constant(g_aff_first, out + 16, g_aff_count);
                } else if (g_aff_count) {
                    d3d8_vsh_set_constant(g_aff_first, out, g_aff_count);
                }
                h->fn(arg);
            }
        } else if (hle_d3d8_op_sets_state_only(h->fn)) {
            h->fn(arg);
        }
        off += sizeof(op_header) + INTERP_ALIGN(h->size);
    }
    {
        static int ready;
        static unsigned long from, to;
        if (!ready) {
            const char *v = getenv("RECOMP_INTERP_ACCOUNT");
            ready = 1;
            if (v && *v && sscanf(v, "%lu-%lu", &from, &to) != 2)
                to = from;
        }
        if (to && recomp_lockstep_tick() >= from && recomp_lockstep_tick() <= to) {
            int n = 0, i;
            double prims = 0.0;
            for (i = 0; i < prev->ndraws && i < g_orphan_cap; i++)
                if (g_orphan[i]) {
                    n++;
                    prims += prev->draws[i].key.prims;
                }
            fprintf(stderr, "[INTERP-FILL] tick %u blend %.2f: %d draws of the frame before "
                    "filled in (%.0f prims) after op %zu\n", recomp_lockstep_tick(), a, n,
                    prims, upto);
        }
    }
    /* This list's state, as it stood */
    d3d8_SetScissorWarp(turn_n, sx, sy);
    for (off = 0; off <= upto && off < cur->len; ) {
        op_header *h = (op_header *)(cur->ops + off);
        if (h->draw < 0 && hle_d3d8_op_sets_state_only(h->fn))
            h->fn(h->size ? (const void *)(h + 1) : NULL);
        off += sizeof(op_header) + INTERP_ALIGN(h->size);
    }
}

static void redraw(double a)
{
    InterpList *cur = &g_list[g_cur];
    const float *c = hle_d3d8_interp_constants();
    int rec = hle_d3d8_interp_rec, warped = 0;
    double turn[9], sx = 0.0, sy = 0.0;
    size_t off;

    if (!xbox_D3D8InterpBegin()) {
        static int said;
        if (!said++) {
            fprintf(stderr, "[INTERP] the renderer has no second target; frame "
                    "interpolation off\n");
            fflush(stderr);
        }
        g_on = 0;
        return;
    }
    hle_d3d8_interp_rec = 0;                     /* what is drawn now is not kept */
    hle_d3d8_interp_replaying = 1;
    d3d8_SetInterpRedraw(1);
    /* The camera's turn from N to this frame, for the scissors: the
     * reference piece's motion a of the way, less all of it. */
    if (g_ref_draw >= 0 && g_proj >= 0 && g_ref_draw < cur->ndraws) {
        const Prepared *p = &g_prep[g_ref_draw * g_groups];
        const float *pr = regs_of(cur, g_ref_draw);
        double ra[9], rd[9];
        int i, j;

        motion_rotation(p, a, ra);
        quat_to(p->q, rd);
        for (i = 0; i < 3; i++)                  /* ra * rd^T */
            for (j = 0; j < 3; j++)
                turn[i * 3 + j] = ra[i * 3 + 0] * rd[j * 3 + 0] + ra[i * 3 + 1] * rd[j * 3 + 1] +
                                  ra[i * 3 + 2] * rd[j * 3 + 2];
        sx = fabs((double)pr[0]);
        sy = fabs((double)pr[5]);
        warped = 1;
        d3d8_SetScissorWarp(turn, sx, sy);
        camera_back(p, a);
    } else {
        g_cam_back_ok = 0;
    }
    for (off = 0; off < cur->len; ) {
        op_header *h = (op_header *)(cur->ops + off);
        if (h->draw >= 0)
            set_draw_registers(h->draw, a);
        h->fn(h->size ? (const void *)(h + 1) : NULL);
        if (warped && g_orphans && (long long)off == g_inject_off && !fill_off())
            fill_pass(a, off, turn, sx, sy);
        off += sizeof(op_header) + INTERP_ALIGN(h->size);
    }
    d3d8_SetScissorWarp(NULL, 0.0, 0.0);
    d3d8_SetInterpRedraw(0);
    hle_d3d8_interp_replaying = 0;
    if (g_dump_prefix && g_swaps + 1 >= g_dump_from && g_dumped < 24) {
        char path[512];
        snprintf(path, sizeof path, "%s_i%05lu_%d.bmp", g_dump_prefix, g_swaps + 1, g_slot);
        if (hle_d3d8_dump_back_buffer(path) == 0) {
            g_dumped++;
            fprintf(stderr, "[INTERP] in-between frame %d before swap %lu (blend %.3f) "
                    "-> %s\n", g_slot, g_swaps + 1, a, path);
        }
    }
    /* Back to frame N's registers as it left them. */
    if (g_proj >= 0)
        d3d8_vsh_set_constant(g_proj, c + g_proj * 4, 4);
    if (g_aff_count)
        d3d8_vsh_set_constant(g_aff_first, c + g_aff_first * 4, g_aff_count);
    hle_d3d8_overlay_redraw();
    {
        /* The moment it goes to the screen. A present can then block --
         * a Vulkan swap chain waits there for an image to draw the next
         * frame into -- but this one is already on its way. */
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_presented_at = t.QuadPart;
    }
    xbox_D3D8InterpPresent();
    xbox_D3D8InterpEnd();
    hle_d3d8_interp_rec = rec;
    g_presents_total++;
    g_shown++;
}

/* The gate's wait, lent (xbox_Nv2aFlipGateSetIdle). In-between frame k of
 * n is due k/n of the way from the last frame shown to the vblank that
 * shows this one; it is started early by what a redraw has been costing,
 * so it lands on time, and given up if it would not be done before the
 * vblank. Its blend is for the moment it is shown. */
static long long gate_idle(long long now, long long release)
{
    LONGLONG span = release - g_last_present;

    if (!g_on || !g_last_present)
        return 0;
    if (!g_prepared) {
        g_prepared = 1;
        g_frame_ok = 0;
        if (hle_d3d8_capture_active() || hle_d3d8_movie_layer_shown())
            g_held_other++;
        else
            g_frame_ok = prepare_frame();
    }
    if (!g_frame_ok || span <= g_qpf / 1000 || span > g_qpf / 10)
        return 0;
    while (g_slot < g_multiple) {
        LONGLONG due = g_last_present + span * g_slot / g_multiple;
        LONGLONG start = due - g_redraw_est;
        LARGE_INTEGER a, b;
        double alpha, ms;

        if (now < start)
            return start;
        if (now + g_redraw_est > release - g_qpf / 2000) {
            g_late += g_multiple - g_slot;       /* too late for the rest */
            g_slot = g_multiple;
            /* Nothing measured, so the estimate cannot correct itself: let
             * it fall back, or one slow redraw would hold every frame. */
            g_redraw_est -= g_redraw_est / 4;
            return 0;
        }
        alpha = (double)((now + g_redraw_est > due ? now + g_redraw_est : due) -
                         g_last_present) / (double)span;
        if (alpha > 1.0)
            alpha = 1.0;
        QueryPerformanceCounter(&a);
        redraw(alpha);
        QueryPerformanceCounter(&b);
        /* What a redraw costs up to its present, followed slowly, and never
         * taken as more than a quarter of the period: a frame that stalled
         * (a pipeline built, a page fault) is not what the next one costs. */
        {
            LONGLONG cost = g_presented_at - a.QuadPart;
            if (cost > span / 4)
                cost = span / 4;
            g_redraw_est = (g_redraw_est * 7 + cost) / 8;
        }
        ms = (double)(g_presented_at - a.QuadPart) * 1000.0 / (double)g_qpf;
        g_redraw_ms_sum += ms;
        g_redraws++;
        if (ms > g_redraw_ms_max)
            g_redraw_ms_max = ms;
        /* How far from its moment it reached the screen. */
        ms = fabs((double)(g_presented_at - due)) * 1000.0 / (double)g_qpf;
        g_off_ms_sum += ms;
        if (ms > g_off_ms_max)
            g_off_ms_max = ms;
        g_alpha_sum += alpha;
        g_slot++;
        if (!g_on)
            return 0;
        now = b.QuadPart;
    }
    return 0;
}

/* ---------------------------------------------------------- frame edges */

/* The registers: what the title or the settings say now. A change drops
 * what was kept, since its draws carry the old ones. */
static void follow_registers(void)
{
    int proj, first, count;

    d3d8_vsh_interp_registers(&proj, &first, &count);
    if (count > INTERP_MAX_AFFINE)
        count = INTERP_MAX_AFFINE;
    if (proj == g_proj && first == g_aff_first && count == g_aff_count && g_nregs)
        return;
    g_proj = proj;
    g_aff_first = first;
    g_aff_count = count;
    g_groups = count / 4;
    g_nregs = (proj >= 0 ? 4 : 0) + count;
    list_retire(&g_list[0]);
    list_retire(&g_list[1]);
    {
        int i;
        for (i = 0; i < 2; i++) {
            free(g_list[i].regs);
            free(g_list[i].draws);
            g_list[i].regs = NULL;
            g_list[i].draws = NULL;
            g_list[i].cap_draws = 0;
        }
    }
    fprintf(stderr, "[INTERP] blending c%d-c%d as the projection, %s%d affine "
            "matrices from c%d\n", proj, proj + 3, proj < 0 ? "(none) " : "",
            g_groups, first);
    fflush(stderr);
}

void hle_d3d8_interp_init(void)
{
    static int done;
    LARGE_INTEGER f;
    int n;

    if (done)
        return;
    done = 1;
    n = recomp_config_int("RECOMP_FRAME_INTERP", "frame_interp", 0);
    if (n < 2)
        return;
    if (n > INTERP_MAX_MULTIPLE)
        n = INTERP_MAX_MULTIPLE;
    if (hle_d3d8_defer_on()) {
        fprintf(stderr, "[INTERP] frame interpolation is not combined with deferred "
                "frames (RECOMP_HLE_D3D8_DEFER); off\n");
        fflush(stderr);
        return;
    }
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    g_redraw_est = g_qpf / 500;                  /* 2 ms until measured */
    g_multiple = n;
    g_on = 1;
    g_dump_prefix = getenv("RECOMP_INTERP_DUMP");
    if (g_dump_prefix && !*g_dump_prefix)
        g_dump_prefix = NULL;
    {
        const char *from = getenv("RECOMP_INTERP_DUMP_FROM");
        g_dump_from = from ? (unsigned long)atol(from) : 0;
    }
    xbox_Nv2aFlipGateSetIdle(gate_idle);
    fprintf(stderr, "[INTERP] frame interpolation: %d frames shown for each the title "
            "draws (RECOMP_FRAME_INTERP)\n", n);
    fflush(stderr);
}

void hle_d3d8_interp_frame_end(void)
{
    LARGE_INTEGER now;

    if (!g_multiple)
        return;
    QueryPerformanceCounter(&now);
    g_last_present = now.QuadPart;
    g_frames++;
    g_swaps++;
    if (!g_on) {
        if (hle_d3d8_interp_rec) {
            hle_d3d8_interp_rec = 0;
            list_retire(&g_list[0]);
            list_retire(&g_list[1]);
        }
        return;
    }
    follow_registers();
    /* The frame just shown becomes the one before; what the one before it
     * put off can happen now, since neither will be drawn again. */
    list_retire(&g_list[g_cur ^ 1]);
    g_cur ^= 1;
    list_retire(&g_list[g_cur]);
    g_list[g_cur].whole = 1;
    hle_d3d8_interp_rec = 1;
    hle_d3d8_interp_snapshot();
    g_prepared = 0;
    g_frame_ok = 0;
    g_slot = 1;
}

void hle_d3d8_interp_report(void)
{
    unsigned long held = g_held_2d + g_held_match + g_held_cut + g_held_other;

    if (!g_multiple)
        return;
    fprintf(stderr, "[INTERP] %dx: %lu frames, %lu in-between shown, %lu late; held "
            "%lu (%lu no 3D, %lu matched too little, %lu cut, %lu other); matched "
            "%.0f%% of 3D primitives; redraw %.2f ms average, %.2f worst; shown "
            "%.2f ms from its moment on average, %.2f worst; blend %.2f average%s\n",
            g_multiple, g_frames, g_shown, g_late, held, g_held_2d, g_held_match,
            g_held_cut, g_held_other,
            g_matched_frames ? g_matched_sum * 100.0 / (double)g_matched_frames : 0.0,
            g_redraws ? g_redraw_ms_sum / (double)g_redraws : 0.0, g_redraw_ms_max,
            g_redraws ? g_off_ms_sum / (double)g_redraws : 0.0, g_off_ms_max,
            g_redraws ? g_alpha_sum / (double)g_redraws : 0.0,
            g_on ? "" : " (off)");
    fflush(stderr);
    g_frames = g_shown = g_late = g_held_2d = g_held_match = g_held_cut = 0;
    g_held_other = 0;
    g_matched_sum = g_redraw_ms_sum = g_redraw_ms_max = 0.0;
    g_alpha_sum = g_off_ms_sum = g_off_ms_max = 0.0;
    g_redraws = g_matched_frames = 0;
}

