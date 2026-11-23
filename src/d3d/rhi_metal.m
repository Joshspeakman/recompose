/**
 * rhi_metal.m -- the renderer's native Metal backend (macOS).
 *
 * The third RhiBackend beside Direct3D 11 and Vulkan, so a Mac draws through
 * Metal itself rather than through MoltenVK and a Vulkan loader. It is
 * selected with RECOMP_D3D8_BACKEND=metal while it is brought up; rhi.c
 * keeps Vulkan the default until the two match capture for capture
 * (docs/technical/vulkan-backend.md, section 5, is the bring-up loop).
 *
 * The shape follows rhi_d3d11.c more than rhi_vulkan.c, because the RHI is
 * D3D11-shaped and so is Metal: an immediate encoder, render state set per
 * draw, resources the command buffer keeps alive by itself.
 *
 * Shaders. The generators write HLSL and keep writing HLSL. A shader is
 * compiled by the same DXC call and with the same binding shifts as the
 * Vulkan backend (rhi_vulkan_dxc.cpp, rhi_vulkan_bindings.h), and found in
 * the same disk cache, so a run that has seen a shader before never loads
 * DXC. SPIRV-Cross, linked statically, turns the SPIR-V into Metal Shading
 * Language, and Metal compiles that. The binding convention becomes Metal
 * argument indices:
 *
 *   vertex b0..b7        -> vertex   [[buffer(0..7)]]
 *   pixel  b0..b7        -> fragment [[buffer(0..7)]]
 *   t0..t15              -> fragment [[texture(0..15)]]
 *   s0..s15              -> fragment [[sampler(0..15)]]
 *   vertex streams 0..15 -> vertex   [[buffer(8..23)]]
 *   zero stream          -> vertex   [[buffer(30)]]  (inputs no layout feeds)
 *
 * Varyings. D3D11 links a vertex and a pixel shader by semantic; Metal links
 * by [[user(locnN)]] and requires the two sides to agree in type. So every
 * varying goes to one fixed location per semantic, in every shader
 * (canonical_location), as a float4: the pixel side is declared float4 by
 * SPIRV-Cross (shader inputs of vecsize 4), the vertex side is widened here
 * (widen_outputs). A pixel shader reading a semantic its vertex shader does
 * not write gets a vertex variant with zero outputs added (pad_variant).
 *
 * Clip space and facing need nothing: Metal's normalised device coordinates,
 * depth range and framebuffer origin are D3D's, so there is no negative
 * viewport (vulkan-backend.md 4.4) and no second winding inversion.
 *
 * Built without ARC: every Objective-C object here is retained and released
 * by hand, and every entry point that can make an autoreleased one runs in
 * its own pool, because the guest threads that call in have none.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "rhi_backend.h"
#include "rhi_shader_cache.h"
#include "rhi_vulkan_bindings.h"

#include <spirv_cross/spirv_cross_c.h>

#include <ctype.h>
#include <float.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_feature(objc_arc)
#error rhi_metal.m manages its own references: build it with -fno-objc-arc
#endif

/* d3d8_compile.c: every backend shows each HLSL source to it first. */
void d3d8_hlsl_note(const char *src, size_t len, const char *name,
                    const RhiMacro *macros, const char *entry, const char *target);
/* rhi_vulkan_dxc.cpp: HLSL -> SPIR-V with the backend binding shifts. */
int rhi_vk_dxc_compile(uint32_t stage, const RhiShaderSource *src,
                       uint32_t **spirv, size_t *spirv_bytes, char *err, size_t err_len);

#define MAX_VB          16
#define VB_BASE         8u          /* vertex stream n -> [[buffer(VB_BASE + n)]] */
#define ZERO_VB         30u
#define RING_PAGE       (8u << 20)
#define FRAMES          2
#define OCCL_SLOTS      8192u

/* The same disk-cache tag as rhi_vulkan.c: one SPIR-V blob serves both. */
#define MTL_CACHE_STR2(x) #x
#define MTL_CACHE_STR(x)  MTL_CACHE_STR2(x)
#define MTL_SPIRV_TAG "vulkan-dxc-2-b" MTL_CACHE_STR(RHI_VK_PS_UNIFORM_BASE) \
                      "-t" MTL_CACHE_STR(RHI_VK_TEXTURE_BASE) "-s" MTL_CACHE_STR(RHI_VK_SAMPLER_BASE)

/* ---- objects ---------------------------------------------------------------------------------- */

enum { SWZ_NONE = 0, SWZ_ALPHA_ONE, SWZ_ARGB4 };

#define MAX_VERSIONS 8
#define BYTES_MAX    4096u      /* setVertexBytes' limit */

/* A constant buffer of up to 4 KB is a CPU copy that each draw hands to
 * Metal with set*Bytes: Metal keeps its own snapshot in the command stream,
 * so the buffer can change between any two draws and nothing outlives a
 * frame. Every other buffer owns a few MTLBuffers and moves to a free one on
 * a discard while the GPU still reads the current one, as a D3D driver
 * renames: a ring that appends with NO_OVERWRITE for many frames keeps the
 * same memory throughout. */
struct RhiBuffer {
    RhiBufferDesc d;
    uint8_t      *bytes;        /* constant buffers: the CPU copy */
    uint32_t      gen;          /* bumped on every write of bytes */
    struct {
        id<MTLBuffer> buf;      /* retained */
        uint64_t      used;     /* serial of the last command buffer that read it */
    } v[MAX_VERSIONS];
    uint32_t      nv, cur;
};

struct RhiImage {
    int            refs;
    RhiImageDesc   d;
    id<MTLTexture> tex;         /* retained */
    MTLPixelFormat pf;
    int            swizzle;     /* SWZ_*: how a sampled view reads it */
    uint64_t       used;
    /* A clear not yet made: folded into the next pass's load action, or
     * made on its own before anything else looks at the image. Only for a
     * single-level, single-layer image (render targets). */
    int            clear_color;
    float          clear_rgba[4];
    uint32_t       clear_ds;    /* RHI_CLEAR_DEPTH | RHI_CLEAR_STENCIL */
    float          clear_z;
    uint8_t        clear_s;
};

struct RhiView {
    RhiImage      *img;
    uint32_t       kind;
    RhiViewDesc    d;
    id<MTLTexture> sampled;     /* retained; SAMPLED views only */
    uint32_t       level, layer;
};

typedef struct {
    char     sem[40];
    uint32_t index, loc;
} IoVar;

typedef struct {
    uint32_t       mask;        /* extra locations padded in */
    id<MTLFunction> fn;
} VsVariant;

enum { TEX_2D = 0, TEX_2D_ARRAY = 1, TEX_CUBE = 2, TEX_3D = 3, TEX_2D_MS = 4 };

struct RhiShader {
    uint32_t        stage;
    uint64_t        id;
    char           *msl;        /* malloc'd; the vertex side keeps it for variants */
    char            entry[128];
    id<MTLFunction> fn;
    IoVar           in[32];     /* vertex: inputs by semantic -> [[attribute(loc)]] */
    uint32_t        nin;
    uint32_t        io_mask;    /* vertex: locations written; pixel: locations read */
    uint32_t        ubo_mask;   /* [[buffer(n)]] constant buffers it reads */
    uint8_t         tex_kind[16];
    uint32_t        tex_mask, smp_mask;
    VsVariant       variants[8];
    uint32_t        nvariants;
};

struct RhiVertexLayout {
    uint64_t         id;
    uint32_t         n;
    RhiVertexElement e[32];
    char             names[32][40];
};

struct RhiBlendState  { RhiBlendDesc d; };
struct RhiRasterState { RhiRasterDesc d; };
struct RhiDepthState  { RhiDepthDesc d; id<MTLDepthStencilState> dss; };
struct RhiSampler     { RhiSamplerDesc d; id<MTLSamplerState> s; };

typedef struct {
    uint64_t       vs, ps, layout;
    uint32_t       strides[MAX_VB];
    MTLPixelFormat color, depth;
    uint32_t       samples;
    uint32_t       blend_enable, src, dst, op, src_a, dst_a, op_a, write_mask, a2c;
} PipeKey;

typedef struct Pipe {
    PipeKey                    key;
    id<MTLRenderPipelineState> state;   /* nil: its draws are skipped */
    struct Pipe               *next;
    uint16_t                   vb_used; /* streams the layout feeds */
} Pipe;

typedef struct Page {
    id<MTLBuffer> buf;
    uint32_t      size, used_bytes;
    uint64_t      serial;               /* last command buffer that read it */
    struct Page  *next;
} Page;

/* ---- the device ------------------------------------------------------------------------------------ */

static struct {
    id<MTLDevice>              dev;
    id<MTLCommandQueue>        q;
    CAMetalLayer              *layer;
    id<MTLCommandBuffer>       cb;
    id<MTLRenderCommandEncoder> enc;
    uint64_t                   serial;      /* the command buffer being recorded */
    _Atomic uint64_t           done;        /* the highest finished */
    id<MTLCommandBuffer>       frame_cb[FRAMES];   /* the last frames presented, oldest first */
    int                        unified;
    uint64_t                   next_id;

    Page                      *page, *pages;

    /* bound state */
    RhiView                   *rt_color, *rt_depth;
    RhiViewport                vp;
    RhiRect                    scissor;
    uint32_t                   topology;
    RhiVertexLayout           *layout;
    struct { RhiBuffer *b; uint32_t stride, offset; } vb[MAX_VB];
    RhiBuffer                 *ib;
    uint32_t                   ib_bits, ib_off;
    RhiShader                 *vs, *ps;
    RhiBuffer                 *ubo[2][8];
    RhiView                   *tex[16];
    RhiSampler                *smp[16];
    RhiBlendState             *blend;
    float                      blend_factor[4];
    RhiDepthState             *depth;
    uint32_t                   stencil_ref;
    RhiRasterState            *raster;

    /* what the open encoder already has */
    struct {
        id<MTLRenderPipelineState> pipe;
        id<MTLDepthStencilState>   dss;
        id<MTLBuffer>              vbuf[32];
        uint32_t                   voff[32];
        id<MTLBuffer>              fbuf[8];
        uint32_t                   foff[8];
        id<MTLTexture>             tex[16];
        id<MTLSamplerState>        smp[16];
        RhiBuffer                 *cb[2][8];       /* constants sent as bytes, and when */
        uint32_t                   cb_gen[2][8];
        uint32_t                   width, height;
        int                        has_color, has_depth;
        MTLPixelFormat             color_pf, depth_pf;
        uint32_t                   samples;
    } e;

    /* the swap chain: a back buffer of our own, copied to a drawable at present */
    RhiImage                  *bb;
    RhiView                   *bb_view;
    int                        display_sync;   /* -1 unknown */

    RhiImage                  *dummy_img[5];
    RhiView                   *dummy[5];
    id<MTLBuffer>              zero;
    id<MTLSamplerState>        def_sampler;
    id<MTLDepthStencilState>   no_depth;

    Pipe                      *pipes[1024];

    /* occlusion counting */
    id<MTLBuffer>              occl;
    uint32_t                   occl_next, occl_gen;
    int                        occl_on;
    uint32_t                   occl_first;
    uint64_t                   occl_serial;    /* last command buffer that counted */

    int                        trace;

    /* what the last stretch of frames cost: printed every 300 presents */
    struct {
        uint32_t presents, submits, sync_waits, renames, new_versions, version_waits,
                 staged_uploads, direct_uploads, pipelines, discards;
    } st;
} M;

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

static void say_once(int *flag, const char *msg)
{
    if (!*flag) {
        *flag = 1;
        fprintf(stderr, "[RHI] metal: %s\n", msg);
        fflush(stderr);
    }
}

/* ---- formats --------------------------------------------------------------------------------------- */

static MTLPixelFormat mtl_format(RhiFormat f, int *swizzle)
{
    int sw = SWZ_NONE;
    MTLPixelFormat p;

    switch (f) {
    case RHI_FORMAT_R32G32B32A32_FLOAT: p = MTLPixelFormatRGBA32Float; break;
    case RHI_FORMAT_R32G32B32A32_SINT:  p = MTLPixelFormatRGBA32Sint; break;
    case RHI_FORMAT_R16G16B16A16_FLOAT: p = MTLPixelFormatRGBA16Float; break;
    case RHI_FORMAT_R16G16B16A16_UNORM: p = MTLPixelFormatRGBA16Unorm; break;
    case RHI_FORMAT_R16G16B16A16_SNORM: p = MTLPixelFormatRGBA16Snorm; break;
    case RHI_FORMAT_R32G32_FLOAT:       p = MTLPixelFormatRG32Float; break;
    /* DXGI and Metal both name packed formats from the low bits up. */
    case RHI_FORMAT_R10G10B10A2_UNORM:  p = MTLPixelFormatRGB10A2Unorm; break;
    case RHI_FORMAT_R11G11B10_FLOAT:    p = MTLPixelFormatRG11B10Float; break;
    case RHI_FORMAT_R8G8B8A8_UNORM:     p = MTLPixelFormatRGBA8Unorm; break;
    case RHI_FORMAT_R8G8B8A8_SNORM:     p = MTLPixelFormatRGBA8Snorm; break;
    case RHI_FORMAT_R16G16_FLOAT:       p = MTLPixelFormatRG16Float; break;
    case RHI_FORMAT_R16G16_UNORM:       p = MTLPixelFormatRG16Unorm; break;
    case RHI_FORMAT_R16G16_SNORM:       p = MTLPixelFormatRG16Snorm; break;
    case RHI_FORMAT_D32_FLOAT:          p = MTLPixelFormatDepth32Float; break;
    case RHI_FORMAT_R32_FLOAT:          p = MTLPixelFormatR32Float; break;
    case RHI_FORMAT_R32_UINT:           p = MTLPixelFormatR32Uint; break;
    /* Apple GPUs have no D24S8; D32S8 holds every value it can. */
    case RHI_FORMAT_D24_UNORM_S8_UINT:  p = MTLPixelFormatDepth32Float_Stencil8; break;
    case RHI_FORMAT_R8G8_UNORM:         p = MTLPixelFormatRG8Unorm; break;
    case RHI_FORMAT_R8G8_SNORM:         p = MTLPixelFormatRG8Snorm; break;
    case RHI_FORMAT_R16_FLOAT:          p = MTLPixelFormatR16Float; break;
    case RHI_FORMAT_D16_UNORM:          p = MTLPixelFormatDepth16Unorm; break;
    case RHI_FORMAT_R16_UNORM:          p = MTLPixelFormatR16Unorm; break;
    case RHI_FORMAT_R16_UINT:           p = MTLPixelFormatR16Uint; break;
    case RHI_FORMAT_R16_SNORM:          p = MTLPixelFormatR16Snorm; break;
    case RHI_FORMAT_R8_UNORM:           p = MTLPixelFormatR8Unorm; break;
    case RHI_FORMAT_A8_UNORM:           p = MTLPixelFormatA8Unorm; break;   /* samples (0,0,0,a) as D3D does */
    case RHI_FORMAT_BC1_UNORM:          p = MTLPixelFormatBC1_RGBA; break;
    case RHI_FORMAT_BC2_UNORM:          p = MTLPixelFormatBC2_RGBA; break;
    case RHI_FORMAT_BC3_UNORM:          p = MTLPixelFormatBC3_RGBA; break;
    case RHI_FORMAT_BC5_UNORM:          p = MTLPixelFormatBC5_RGUnorm; break;
    case RHI_FORMAT_B5G6R5_UNORM:       p = MTLPixelFormatB5G6R5Unorm; break;
    case RHI_FORMAT_B5G5R5A1_UNORM:     p = MTLPixelFormatBGR5A1Unorm; break;
    case RHI_FORMAT_B8G8R8A8_UNORM:     p = MTLPixelFormatBGRA8Unorm; break;
    case RHI_FORMAT_B8G8R8X8_UNORM:     p = MTLPixelFormatBGRA8Unorm; sw = SWZ_ALPHA_ONE; break;
    /* DXGI B4G4R4A4 is B in bits 0-3 .. A in 12-15; Metal's ABGR4 reads
     * those bits as A, B, G, R. The view swizzles them back. */
    case RHI_FORMAT_B4G4R4A4_UNORM:     p = MTLPixelFormatABGR4Unorm; sw = SWZ_ARGB4; break;
    default:                            p = MTLPixelFormatInvalid; break;
    }
    if (swizzle)
        *swizzle = sw;
    return p;
}

static int is_depth_pf(MTLPixelFormat p)
{
    return p == MTLPixelFormatDepth32Float || p == MTLPixelFormatDepth16Unorm ||
           p == MTLPixelFormatDepth32Float_Stencil8;
}

static int has_stencil_pf(MTLPixelFormat p)
{
    return p == MTLPixelFormatDepth32Float_Stencil8;
}

static MTLVertexFormat mtl_vertex_format(RhiFormat f)
{
    switch (f) {
    case RHI_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
    case RHI_FORMAT_R32G32B32A32_SINT:  return MTLVertexFormatInt4;
    case RHI_FORMAT_R32G32B32_FLOAT:    return MTLVertexFormatFloat3;
    case RHI_FORMAT_R32G32_FLOAT:       return MTLVertexFormatFloat2;
    case RHI_FORMAT_R32_FLOAT:          return MTLVertexFormatFloat;
    case RHI_FORMAT_R32_UINT:           return MTLVertexFormatUInt;
    case RHI_FORMAT_R16G16B16A16_FLOAT: return MTLVertexFormatHalf4;
    case RHI_FORMAT_R16G16B16A16_UNORM: return MTLVertexFormatUShort4Normalized;
    case RHI_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
    case RHI_FORMAT_R16G16_FLOAT:       return MTLVertexFormatHalf2;
    case RHI_FORMAT_R16G16_UNORM:       return MTLVertexFormatUShort2Normalized;
    case RHI_FORMAT_R16G16_SNORM:       return MTLVertexFormatShort2Normalized;
    case RHI_FORMAT_R16_FLOAT:          return MTLVertexFormatHalf;
    case RHI_FORMAT_R16_UNORM:          return MTLVertexFormatUShortNormalized;
    case RHI_FORMAT_R16_SNORM:          return MTLVertexFormatShortNormalized;
    case RHI_FORMAT_R16_UINT:           return MTLVertexFormatUShort;
    case RHI_FORMAT_R10G10B10A2_UNORM:  return MTLVertexFormatUInt1010102Normalized;
    case RHI_FORMAT_R11G11B10_FLOAT:    return MTLVertexFormatFloatRG11B10;
    case RHI_FORMAT_R8G8B8A8_UNORM:     return MTLVertexFormatUChar4Normalized;
    case RHI_FORMAT_R8G8B8A8_SNORM:     return MTLVertexFormatChar4Normalized;
    case RHI_FORMAT_B8G8R8A8_UNORM:     return MTLVertexFormatUChar4Normalized_BGRA;
    case RHI_FORMAT_R8G8_UNORM:         return MTLVertexFormatUChar2Normalized;
    case RHI_FORMAT_R8G8_SNORM:         return MTLVertexFormatChar2Normalized;
    case RHI_FORMAT_R8_UNORM:           return MTLVertexFormatUCharNormalized;
    default:                            return MTLVertexFormatInvalid;
    }
}

/* ---- command buffers and serials ------------------------------------------------------------------- */

static uint64_t completed(void) { return atomic_load(&M.done); }

static id<MTLCommandBuffer> cmd(void)
{
    if (!M.cb) {
        @autoreleasepool {
            M.cb = [[M.q commandBuffer] retain];
        }
    }
    return M.cb;
}

static void end_encoder(void)
{
    if (M.enc) {
        [M.enc endEncoding];
        [M.enc release];
        M.enc = nil;
    }
}

/* Sends what has been recorded. wait: block until the GPU has finished it. */
static void submit(int wait, id<CAMetalDrawable> drawable)
{
    id<MTLCommandBuffer> cb;
    uint64_t serial;

    end_encoder();
    cb = cmd();
    serial = M.serial;
    if (drawable)
        [cb presentDrawable:drawable];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        uint64_t cur = atomic_load(&M.done);
        while (cur < serial && !atomic_compare_exchange_weak(&M.done, &cur, serial)) {}
        if (done.status == MTLCommandBufferStatusError) {
            static int said;
            if (said++ < 5)
                fprintf(stderr, "[RHI] metal: a command buffer failed: %s\n",
                        done.error.localizedDescription.UTF8String);
        }
    }];
    [cb commit];
    if (drawable) {
        /* Frame pacing waits on the command buffer itself (a kernel wait,
         * like a Vulkan fence), not on a completion handler: under CPU
         * contention the handler's thread runs late, and a frame the GPU
         * finished long ago would still hold the game back. */
        int k;
        if (M.frame_cb[0]) {
            if (M.frame_cb[0].status < MTLCommandBufferStatusCompleted) {
                int h = rhi_wait_begin();
                [M.frame_cb[0] waitUntilCompleted];
                rhi_wait_end(h);
            }
            [M.frame_cb[0] release];
        }
        for (k = 0; k + 1 < FRAMES; k++)
            M.frame_cb[k] = M.frame_cb[k + 1];
        M.frame_cb[FRAMES - 1] = [cb retain];
    }
    M.cb = nil;
    M.serial++;
    M.st.submits++;
    if (wait)
        M.st.sync_waits++;
    if (M.page)
        M.page->serial = serial;
    if (wait) {
        int h = rhi_wait_begin();
        [cb waitUntilCompleted];
        rhi_wait_end(h);
    }
    [cb release];
}

static void wait_serial(uint64_t s)
{
    if (s >= M.serial)              /* still being recorded */
        submit(1, nil);
    else if (completed() < s) {
        submit(1, nil);             /* everything before it is ahead of an empty buffer */
    }
}

/* ---- ring pages: dynamic buffers and staging --------------------------------------------------------- */

static Page *new_page(uint32_t size)
{
    Page *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->size = size;
    p->buf = [M.dev newBufferWithLength:size options:MTLResourceStorageModeShared |
                                                     MTLResourceCPUCacheModeWriteCombined];
    if (!p->buf) {
        free(p);
        return NULL;
    }
    p->next = M.pages;
    M.pages = p;
    return p;
}

/* bytes of shared memory the GPU reads by the end of the command buffer being
 * recorded; NULL if none can be had. */
static uint8_t *ring_alloc(uint32_t size, uint32_t align, id<MTLBuffer> *buf, uint32_t *off)
{
    Page *p = M.page;
    uint32_t at;

    if (!align)
        align = 256;
    if (p) {
        at = (p->used_bytes + align - 1) & ~(align - 1);
        if (at + size <= p->size) {
            p->used_bytes = at + size;
            p->serial = M.serial;
            *buf = p->buf;
            *off = at;
            return (uint8_t *)[p->buf contents] + at;
        }
        p->serial = M.serial;
    }
    /* a page the GPU has finished with, or a new one */
    for (p = M.pages; p; p = p->next)
        if (p != M.page && p->serial < M.serial && p->serial <= completed() && p->size >= size)
            break;
    if (!p)
        p = new_page(size > RING_PAGE ? (size + 0xFFFFu) & ~0xFFFFu : RING_PAGE);
    if (!p)
        return NULL;
    p->used_bytes = size;
    p->serial = M.serial;
    M.page = p;
    *buf = p->buf;
    *off = 0;
    return (uint8_t *)[p->buf contents];
}

/* ---- images ------------------------------------------------------------------------------------------ */

static void flush_clear(RhiImage *img);

static int simple_target(const RhiImage *img)
{
    return img->d.mip_levels <= 1 && img->d.type == RHI_IMAGE_2D && img->d.depth <= 1 && !img->d.cube;
}

static MTLTextureType tex_type(const RhiImageDesc *d)
{
    if (d->type == RHI_IMAGE_3D)
        return MTLTextureType3D;
    if (d->samples > 1)
        return MTLTextureType2DMultisample;
    if (d->cube)
        return d->depth > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    return d->depth > 1 ? MTLTextureType2DArray : MTLTextureType2D;
}

static void sub_region(const RhiImage *img, uint32_t sub, const RhiBox *box,
                       uint32_t *level, uint32_t *slice, MTLRegion *r)
{
    uint32_t mips = img->d.mip_levels ? img->d.mip_levels : 1;
    uint32_t w, h, dd;

    *level = sub % mips;
    *slice = img->d.type == RHI_IMAGE_3D ? 0 : sub / mips;
    w = img->d.width >> *level;   if (!w) w = 1;
    h = img->d.height >> *level;  if (!h) h = 1;
    dd = img->d.type == RHI_IMAGE_3D ? (img->d.depth >> *level) : 1;
    if (!dd) dd = 1;
    if (box)
        *r = MTLRegionMake3D(box->left, box->top, box->front, box->right - box->left,
                             box->bottom - box->top, box->back > box->front ? box->back - box->front : 1);
    else
        *r = MTLRegionMake3D(0, 0, 0, w, h, dd);
}

static void upload(RhiImage *img, uint32_t sub, const RhiBox *box, const void *data,
                   uint32_t row_pitch, uint32_t slice_pitch)
{
    uint32_t level, slice, rows, bytes, tight;
    MTLRegion r;

    sub_region(img, sub, box, &level, &slice, &r);
    tight = rhi_format_row_pitch(img->d.format, (uint32_t)r.size.width);
    if (!row_pitch)
        row_pitch = tight;
    rows = rhi_format_rows(img->d.format, (uint32_t)r.size.height);
    if (!slice_pitch)
        slice_pitch = row_pitch * rows;
    bytes = slice_pitch * (uint32_t)r.size.depth;

    /* Straight into the texture when nothing on the GPU can still read it:
     * no pass break, no copy. */
    if (img->tex.storageMode != MTLStorageModePrivate &&
        img->used < M.serial && img->used <= completed()) {
        M.st.direct_uploads++;
        [img->tex replaceRegion:r mipmapLevel:level slice:slice withBytes:data
                    bytesPerRow:row_pitch
                  bytesPerImage:img->d.type == RHI_IMAGE_3D ? slice_pitch : 0];
        return;
    }
    /* Otherwise in order with the work recorded so far. */
    @autoreleasepool {
        id<MTLBuffer> buf;
        uint32_t off;
        uint8_t *p = ring_alloc(bytes, 256, &buf, &off);
        id<MTLBlitCommandEncoder> bl;
        if (!p)
            return;
        M.st.staged_uploads++;
        memcpy(p, data, bytes);
        end_encoder();
        bl = [cmd() blitCommandEncoder];
        [bl copyFromBuffer:buf sourceOffset:off sourceBytesPerRow:row_pitch
         sourceBytesPerImage:slice_pitch sourceSize:r.size toTexture:img->tex
         destinationSlice:slice destinationLevel:level destinationOrigin:r.origin];
        [bl endEncoding];
        img->used = M.serial;
    }
}

static RhiImage *m_image_create(const RhiImageDesc *d, const RhiSubresourceData *initial)
{
    MTLTextureDescriptor *td;
    RhiImage *img;
    int sw;
    MTLPixelFormat pf = mtl_format(d->format, &sw);
    uint32_t mips = d->mip_levels ? d->mip_levels : 1, layers, i;

    if (pf == MTLPixelFormatInvalid) {
        fprintf(stderr, "[RHI] metal: no Metal format for DXGI format %u\n", (unsigned)d->format);
        return NULL;
    }
    img = calloc(1, sizeof *img);
    if (!img)
        return NULL;
    img->refs = 1;
    img->d = *d;
    img->d.mip_levels = mips;
    img->pf = pf;
    img->swizzle = sw;

    td = [[MTLTextureDescriptor alloc] init];
    td.textureType = tex_type(d);
    td.pixelFormat = pf;
    td.width = d->width;
    td.height = d->height;
    if (d->type == RHI_IMAGE_3D) {
        td.depth = d->depth ? d->depth : 1;
    } else if (d->cube) {
        td.arrayLength = d->depth > 6 ? d->depth / 6 : 1;
    } else {
        td.arrayLength = d->depth ? d->depth : 1;
    }
    td.mipmapLevelCount = mips;
    td.sampleCount = d->samples > 1 ? d->samples : 1;
    td.usage = MTLTextureUsageShaderRead;
    if (d->bind & (RHI_BIND_RENDER_TARGET | RHI_BIND_DEPTH))
        td.usage |= MTLTextureUsageRenderTarget;
    if (sw != SWZ_NONE)
        td.usage |= MTLTextureUsagePixelFormatView;
    if ((d->bind & (RHI_BIND_RENDER_TARGET | RHI_BIND_DEPTH)) || d->samples > 1 || is_depth_pf(pf))
        td.storageMode = MTLStorageModePrivate;
    else
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        /* Managed only where memory is not unified (an Intel Mac's GPU) */
        td.storageMode = M.unified ? MTLStorageModeShared : MTLStorageModeManaged;
#pragma clang diagnostic pop
    img->tex = [M.dev newTextureWithDescriptor:td];
    [td release];
    if (!img->tex) {
        fprintf(stderr, "[RHI] metal: could not make a %ux%u texture of DXGI format %u\n",
                d->width, d->height, (unsigned)d->format);
        free(img);
        return NULL;
    }
    if (initial) {
        layers = d->type == RHI_IMAGE_3D ? 1 : (d->depth ? d->depth : 1);
        for (i = 0; i < mips * layers; i++)
            if (initial[i].data)
                upload(img, i, NULL, initial[i].data, initial[i].row_pitch, initial[i].slice_pitch);
    }
    return img;
}

static RhiImage *m_image_retain(RhiImage *img)
{
    if (img)
        img->refs++;
    return img;
}

static void m_image_destroy(RhiImage *img)
{
    if (!img || --img->refs > 0)
        return;
    [img->tex release];          /* a command buffer that reads it holds its own reference */
    free(img);
}

static void m_image_get_desc(const RhiImage *img, RhiImageDesc *out) { *out = img->d; }

static void m_image_update(RhiImage *img, uint32_t sub, const RhiBox *box, const void *data,
                           uint32_t row_pitch, uint32_t slice_pitch)
{
    if (!img || !data)
        return;
    if (img->clear_color || img->clear_ds) {
        if (box)
            flush_clear(img);
        else
            img->clear_color = 0, img->clear_ds = 0;   /* overwritten whole */
    }
    upload(img, sub, box, data, row_pitch, slice_pitch);
}

static int m_sample_count_supported(RhiFormat f, uint32_t samples)
{
    MTLPixelFormat pf = mtl_format(f, NULL);
    return pf != MTLPixelFormatInvalid && [M.dev supportsTextureSampleCount:samples];
}

/* ---- views ------------------------------------------------------------------------------------------- */

static RhiView *m_view_create(RhiImage *img, uint32_t kind, const RhiViewDesc *desc)
{
    RhiView *v;
    RhiViewDesc d;

    if (!img)
        return NULL;
    v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    if (desc) {
        d = *desc;
    } else {
        memset(&d, 0, sizeof d);
        d.dim = img->d.type == RHI_IMAGE_3D ? RHI_VIEW_DIM_3D :
                img->d.samples > 1 ? RHI_VIEW_DIM_2D_MS :
                img->d.cube ? RHI_VIEW_DIM_CUBE :
                img->d.depth > 1 ? RHI_VIEW_DIM_2D_ARRAY : RHI_VIEW_DIM_2D;
        d.mip_count = img->d.mip_levels;
        d.layer_count = img->d.type == RHI_IMAGE_3D ? 1 : (img->d.depth ? img->d.depth : 1);
    }
    if (!d.mip_count)
        d.mip_count = img->d.mip_levels - d.base_mip;
    if (!d.layer_count)
        d.layer_count = 1;
    v->img = m_image_retain(img);
    v->kind = kind;
    v->d = d;
    v->level = d.base_mip;
    v->layer = d.base_layer;

    if (kind == RHI_VIEW_SAMPLED) {
        int sw = img->swizzle;
        MTLPixelFormat pf = img->pf;
        MTLTextureType tt;
        uint32_t layers_total = img->d.type == RHI_IMAGE_3D ? 1 : (img->d.depth ? img->d.depth : 1);
        if (d.format != RHI_FORMAT_UNKNOWN && d.format != img->d.format) {
            MTLPixelFormat want = mtl_format(d.format, &sw);
            if (want != MTLPixelFormatInvalid)
                pf = want;
        }
        switch (d.dim) {
        case RHI_VIEW_DIM_2D_ARRAY: tt = MTLTextureType2DArray; break;
        case RHI_VIEW_DIM_2D_MS:    tt = MTLTextureType2DMultisample; break;
        case RHI_VIEW_DIM_CUBE:     tt = MTLTextureTypeCube; break;
        case RHI_VIEW_DIM_3D:       tt = MTLTextureType3D; break;
        default:                    tt = MTLTextureType2D; break;
        }
        if (pf == img->pf && sw == SWZ_NONE && tt == img->tex.textureType &&
            d.base_mip == 0 && d.mip_count >= img->d.mip_levels &&
            d.base_layer == 0 && d.layer_count >= layers_total) {
            v->sampled = [img->tex retain];
        } else {
            MTLTextureSwizzleChannels s = MTLTextureSwizzleChannelsDefault;
            uint32_t layers = tt == MTLTextureType3D ? 1 : d.layer_count;
            if (sw == SWZ_ALPHA_ONE)
                s = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                  MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
            else if (sw == SWZ_ARGB4)
                s = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
                                                  MTLTextureSwizzleAlpha, MTLTextureSwizzleRed);
            if (tt == MTLTextureType2D && layers > 1)
                layers = 1;
            v->sampled = [img->tex newTextureViewWithPixelFormat:pf textureType:tt
                                    levels:NSMakeRange(d.base_mip, d.mip_count)
                                    slices:NSMakeRange(d.base_layer, layers)
                                   swizzle:s];
            if (!v->sampled) {
                static int said;
                say_once(&said, "a texture view could not be made; the whole texture is sampled instead");
                v->sampled = [img->tex retain];
            }
        }
    }
    return v;
}

static void m_view_destroy(RhiView *v)
{
    int i;

    if (!v)
        return;
    if (M.rt_color == v) M.rt_color = NULL;
    if (M.rt_depth == v) M.rt_depth = NULL;
    for (i = 0; i < 16; i++)
        if (M.tex[i] == v)
            M.tex[i] = NULL;
    [v->sampled release];
    m_image_destroy(v->img);
    free(v);
}

static RhiImage *m_view_image(const RhiView *v) { return v ? v->img : NULL; }

/* ---- render passes ------------------------------------------------------------------------------------ */

static int pass_open_on(RhiImage *img)
{
    return M.enc && ((M.rt_color && M.rt_color->img == img) || (M.rt_depth && M.rt_depth->img == img));
}

static void set_attachment(MTLRenderPassAttachmentDescriptor *a, RhiView *v)
{
    a.texture = v->img->tex;
    a.level = v->level;
    if (v->img->d.type == RHI_IMAGE_3D)
        a.depthPlane = v->layer;
    else
        a.slice = v->layer;
    a.storeAction = MTLStoreActionStore;
    a.loadAction = MTLLoadActionLoad;
}

/* Makes the clears an image is waiting for, on their own. */
static void flush_clear(RhiImage *img)
{
    if (!img || (!img->clear_color && !img->clear_ds))
        return;
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        end_encoder();
        if (img->clear_color) {
            rp.colorAttachments[0].texture = img->tex;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(img->clear_rgba[0], img->clear_rgba[1],
                                                                  img->clear_rgba[2], img->clear_rgba[3]);
        }
        if (img->clear_ds && is_depth_pf(img->pf)) {
            rp.depthAttachment.texture = img->tex;
            rp.depthAttachment.loadAction = (img->clear_ds & RHI_CLEAR_DEPTH) ? MTLLoadActionClear
                                                                               : MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = img->clear_z;
            if (has_stencil_pf(img->pf)) {
                rp.stencilAttachment.texture = img->tex;
                rp.stencilAttachment.loadAction = (img->clear_ds & RHI_CLEAR_STENCIL) ? MTLLoadActionClear
                                                                                       : MTLLoadActionLoad;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
                rp.stencilAttachment.clearStencil = img->clear_s;
            }
        }
        img->clear_color = 0;
        img->clear_ds = 0;
        e = [cmd() renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
        img->used = M.serial;
    }
}

static void reset_encoder_cache(void)
{
    memset(&M.e.pipe, 0, (char *)&M.e.width - (char *)&M.e.pipe);
}

/* Opens a pass on the bound targets. 0 when there is nothing to draw to. */
static int begin_pass(void)
{
    RhiView *c = M.rt_color, *d = M.rt_depth;

    if (M.enc)
        return 1;
    if (!c && !d)
        return 0;
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        if (c) {
            RhiImage *img = c->img;
            set_attachment(rp.colorAttachments[0], c);
            if (img->clear_color && c->level == 0 && c->layer == 0) {
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(img->clear_rgba[0], img->clear_rgba[1],
                                                                      img->clear_rgba[2], img->clear_rgba[3]);
                img->clear_color = 0;
            }
            img->used = M.serial;
        }
        if (d) {
            RhiImage *img = d->img;
            set_attachment(rp.depthAttachment, d);
            if ((img->clear_ds & RHI_CLEAR_DEPTH) && d->level == 0 && d->layer == 0) {
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = img->clear_z;
            }
            if (has_stencil_pf(img->pf)) {
                set_attachment(rp.stencilAttachment, d);
                if ((img->clear_ds & RHI_CLEAR_STENCIL) && d->level == 0 && d->layer == 0) {
                    rp.stencilAttachment.loadAction = MTLLoadActionClear;
                    rp.stencilAttachment.clearStencil = img->clear_s;
                }
            }
            img->clear_ds = 0;
            img->used = M.serial;
        }
        if (M.occl)
            rp.visibilityResultBuffer = M.occl;
        M.enc = [[cmd() renderCommandEncoderWithDescriptor:rp] retain];
    }
    if (!M.enc)
        return 0;
    reset_encoder_cache();
    M.e.has_color = c != NULL;
    M.e.has_depth = d != NULL;
    M.e.color_pf = c ? c->img->pf : MTLPixelFormatInvalid;
    M.e.depth_pf = d ? d->img->pf : MTLPixelFormatInvalid;
    M.e.samples = c ? (c->img->d.samples > 1 ? c->img->d.samples : 1)
                    : (d->img->d.samples > 1 ? d->img->d.samples : 1);
    {
        RhiView *v = c ? c : d;
        uint32_t w = v->img->d.width >> v->level, h = v->img->d.height >> v->level;
        if (c && d) {
            /* D3D allows a larger depth buffer; Metal draws the overlap. */
            uint32_t dw = d->img->d.width >> d->level, dh = d->img->d.height >> d->level;
            if (dw < w) w = dw;
            if (dh < h) h = dh;
        }
        M.e.width = w ? w : 1;
        M.e.height = h ? h : 1;
    }
    return 1;
}

static void m_set_render_target(RhiView *color, RhiView *depth)
{
    if (color == M.rt_color && depth == M.rt_depth)
        return;
    end_encoder();
    M.rt_color = color;
    M.rt_depth = depth;
}

static void m_output_save(RhiOutputState *s)
{
    memset(s, 0, sizeof *s);
    s->color = M.rt_color;
    s->depth = M.rt_depth;
    s->viewport_count = 1;
    s->viewports[0] = M.vp;
}

static void m_output_restore(RhiOutputState *s)
{
    m_set_render_target((RhiView *)s->color, (RhiView *)s->depth);
    if (s->viewport_count)
        M.vp = s->viewports[0];
}

static int m_output_color_is(const RhiOutputState *s, const RhiView *v) { return s->color == v; }

static void m_set_viewports(uint32_t n, const RhiViewport *vps)
{
    if (n)
        M.vp = vps[0];
}

static void m_set_scissor(const RhiRect *r)                  { M.scissor = *r; }
static void m_set_topology(uint32_t t)                       { M.topology = t; }
static void m_set_vertex_layout(RhiVertexLayout *l)          { M.layout = l; }

static void m_set_vertex_buffer(uint32_t slot, RhiBuffer *b, uint32_t stride, uint32_t offset)
{
    if (slot >= MAX_VB)
        return;
    M.vb[slot].b = b;
    M.vb[slot].stride = stride;
    M.vb[slot].offset = offset;
}

static void m_set_index_buffer(RhiBuffer *b, uint32_t bits, uint32_t offset)
{
    M.ib = b;
    M.ib_bits = bits;
    M.ib_off = offset;
}

static void m_set_shader(uint32_t stage, RhiShader *s)
{
    if (stage == RHI_STAGE_VERTEX) M.vs = s; else M.ps = s;
}

static void m_set_uniform_buffers(uint32_t stage, uint32_t slot, uint32_t n, RhiBuffer *const *b)
{
    uint32_t i, st = stage == RHI_STAGE_VERTEX ? 0 : 1;
    for (i = 0; i < n && slot + i < 8; i++)
        M.ubo[st][slot + i] = b ? b[i] : NULL;
}

static void m_set_textures(uint32_t slot, uint32_t n, RhiView *const *v)
{
    uint32_t i;
    for (i = 0; i < n && slot + i < 16; i++)
        M.tex[slot + i] = v ? v[i] : NULL;
}

static void m_set_samplers(uint32_t slot, uint32_t n, RhiSampler *const *s)
{
    uint32_t i;
    for (i = 0; i < n && slot + i < 16; i++)
        M.smp[slot + i] = s ? s[i] : NULL;
}

static void m_set_blend_state(RhiBlendState *s, const float *factor, uint32_t mask)
{
    static int said;
    M.blend = s;
    if (factor)
        memcpy(M.blend_factor, factor, sizeof M.blend_factor);
    else
        M.blend_factor[0] = M.blend_factor[1] = M.blend_factor[2] = M.blend_factor[3] = 1.0f;
    if (mask != 0xFFFFFFFFu && mask != 0)
        say_once(&said, "a partial sample mask is ignored (Metal has no dynamic sample mask)");
}

static void m_set_depth_state(RhiDepthState *s, uint32_t ref) { M.depth = s; M.stencil_ref = ref; }
static void m_set_raster_state(RhiRasterState *s)             { M.raster = s; }

/* ---- clears ------------------------------------------------------------------------------------------ */

static void m_clear_color(RhiView *v, const float *rgba)
{
    RhiImage *img;

    if (!v)
        return;
    img = v->img;
    if (pass_open_on(img))
        end_encoder();
    if (simple_target(img) && v->level == 0 && v->layer == 0) {
        img->clear_color = 1;
        memcpy(img->clear_rgba, rgba, sizeof img->clear_rgba);
        return;
    }
    /* a level or layer of a larger image: cleared now, in its own pass */
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        end_encoder();
        set_attachment(rp.colorAttachments[0], v);
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
        e = [cmd() renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
        img->used = M.serial;
    }
}

static void m_clear_depth(RhiView *v, uint32_t flags, float z, uint8_t s)
{
    RhiImage *img;

    if (!v || !flags)
        return;
    img = v->img;
    if (pass_open_on(img))
        end_encoder();
    if (simple_target(img) && v->level == 0 && v->layer == 0) {
        img->clear_ds |= flags;
        if (flags & RHI_CLEAR_DEPTH) img->clear_z = z;
        if (flags & RHI_CLEAR_STENCIL) img->clear_s = s;
        return;
    }
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        end_encoder();
        set_attachment(rp.depthAttachment, v);
        if (flags & RHI_CLEAR_DEPTH) {
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = z;
        }
        if (has_stencil_pf(img->pf)) {
            set_attachment(rp.stencilAttachment, v);
            if (flags & RHI_CLEAR_STENCIL) {
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = s;
            }
        }
        e = [cmd() renderCommandEncoderWithDescriptor:rp];
        [e endEncoding];
        img->used = M.serial;
    }
}

/* ---- buffers ----------------------------------------------------------------------------------------- */

static int by_bytes(const RhiBufferDesc *d)
{
    return (d->bind & RHI_BIND_UNIFORM) && d->size <= BYTES_MAX;
}

static int version_busy(uint64_t used)
{
    return used && (used >= M.serial || used > completed());
}

/* The current version, after a discard: one the GPU is not reading. */
static int rename_version(RhiBuffer *b)
{
    uint32_t i, size = b->d.size ? b->d.size : 4;

    M.st.discards++;
    if (!version_busy(b->v[b->cur].used))
        return 1;
    M.st.renames++;
    for (i = 0; i < b->nv; i++)
        if (!version_busy(b->v[i].used)) {
            b->cur = i;
            return 1;
        }
    M.st.new_versions++;
    if (b->nv < MAX_VERSIONS) {
        id<MTLBuffer> nb = [M.dev newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (nb) {
            b->v[b->nv].buf = nb;
            b->v[b->nv].used = 0;
            b->cur = b->nv++;
            return 1;
        }
    }
    /* every version in flight: wait for the oldest */
    M.st.new_versions--;
    M.st.version_waits++;
    {
        uint32_t oldest = 0;
        for (i = 1; i < b->nv; i++)
            if (b->v[i].used < b->v[oldest].used)
                oldest = i;
        wait_serial(b->v[oldest].used);
        b->cur = oldest;
    }
    return 1;
}

static RhiBuffer *m_buffer_create(const RhiBufferDesc *d, const void *initial)
{
    RhiBuffer *b = calloc(1, sizeof *b);
    uint32_t size = d->size ? d->size : 4;

    if (!b)
        return NULL;
    b->d = *d;
    if (by_bytes(d)) {
        b->bytes = calloc(1, size);
        if (!b->bytes) {
            free(b);
            return NULL;
        }
        if (initial)
            memcpy(b->bytes, initial, d->size);
        b->gen = 1;
        return b;
    }
    b->v[0].buf = initial ? [M.dev newBufferWithBytes:initial length:size options:MTLResourceStorageModeShared]
                          : [M.dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    if (!b->v[0].buf) {
        free(b);
        return NULL;
    }
    b->nv = 1;
    return b;
}

static void m_buffer_destroy(RhiBuffer *b)
{
    uint32_t i;
    int k;
    if (!b)
        return;
    for (i = 0; i < MAX_VB; i++)
        if (M.vb[i].b == b)
            M.vb[i].b = NULL;
    if (M.ib == b)
        M.ib = NULL;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 8; i++)
            if (M.ubo[k][i] == b)
                M.ubo[k][i] = NULL;
    for (i = 0; i < b->nv; i++)
        [b->v[i].buf release];      /* a command buffer reading one holds its own reference */
    free(b->bytes);
    free(b);
}

static void *m_buffer_map(RhiBuffer *b, uint32_t mode)
{
    if (!b)
        return NULL;
    if (b->bytes) {
        b->gen++;
        return b->bytes;
    }
    if (mode == RHI_MAP_WRITE_DISCARD)
        rename_version(b);
    /* NO_OVERWRITE: the same memory, written where the GPU is not reading */
    return [b->v[b->cur].buf contents];
}

static void m_buffer_unmap(RhiBuffer *b) { (void)b; }

static void m_buffer_update(RhiBuffer *b, const void *data)
{
    if (!b || !data)
        return;
    if (b->bytes) {
        memcpy(b->bytes, data, b->d.size);
        b->gen++;
        return;
    }
    rename_version(b);
    memcpy([b->v[b->cur].buf contents], data, b->d.size);
}

/* The MTLBuffer a draw reads now, marked as read by this command buffer. */
static id<MTLBuffer> buffer_now(RhiBuffer *b)
{
    b->v[b->cur].used = M.serial;
    return b->v[b->cur].buf;
}

/* ---- readback and copies ----------------------------------------------------------------------------- */

static int m_image_readback(RhiImage *img, uint32_t sub, void *dst, uint32_t dst_pitch)
{
    uint32_t level, slice, pitch, rows, y;
    MTLRegion r;
    id<MTLTexture> src;
    int rc = -1;

    if (!img || !dst)
        return -1;
    if (is_depth_pf(img->pf) && img->pf != MTLPixelFormatDepth32Float)
        return -1;      /* D32S8 holds a D24S8: no byte-exact readback */
    flush_clear(img);
    sub_region(img, sub, NULL, &level, &slice, &r);
    pitch = rhi_format_row_pitch(img->d.format, (uint32_t)r.size.width);
    rows = rhi_format_rows(img->d.format, (uint32_t)r.size.height);
    if (!pitch || !rows)
        return -1;
    @autoreleasepool {
        id<MTLBuffer> out = [M.dev newBufferWithLength:(NSUInteger)pitch * rows
                                               options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> bl;
        id<MTLTexture> resolved = nil;
        if (!out)
            return -1;
        end_encoder();
        src = img->tex;
        if (img->d.samples > 1) {
            /* resolve first, through an empty pass */
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:img->pf width:r.size.width
                                                                  height:r.size.height mipmapped:NO];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            id<MTLRenderCommandEncoder> e;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            resolved = [M.dev newTextureWithDescriptor:td];
            rp.colorAttachments[0].texture = img->tex;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            rp.colorAttachments[0].resolveTexture = resolved;
            e = [cmd() renderCommandEncoderWithDescriptor:rp];
            [e endEncoding];
            src = resolved;
            level = 0;
            slice = 0;
        }
        bl = [cmd() blitCommandEncoder];
        [bl copyFromTexture:src sourceSlice:slice sourceLevel:level sourceOrigin:r.origin
                 sourceSize:r.size toBuffer:out destinationOffset:0 destinationBytesPerRow:pitch
       destinationBytesPerImage:(NSUInteger)pitch * rows];
        [bl endEncoding];
        img->used = M.serial;
        submit(1, nil);
        {
            const uint8_t *p = [out contents];
            for (y = 0; y < rows; y++)
                memcpy((uint8_t *)dst + (size_t)y * dst_pitch, p + (size_t)y * pitch,
                       pitch < dst_pitch || !dst_pitch ? pitch : dst_pitch);
        }
        rc = 0;
        [resolved release];
        [out release];
    }
    return rc;
}

static int m_image_copy(RhiImage *dst, RhiImage *src)
{
    if (!dst || !src || dst->d.width != src->d.width || dst->d.height != src->d.height ||
        dst->pf != src->pf || dst->d.samples > 1 || src->d.samples > 1)
        return -1;
    flush_clear(src);
    dst->clear_color = 0;
    dst->clear_ds = 0;
    @autoreleasepool {
        id<MTLBlitCommandEncoder> bl;
        end_encoder();
        bl = [cmd() blitCommandEncoder];
        [bl copyFromTexture:src->tex sourceSlice:0 sourceLevel:0 toTexture:dst->tex
           destinationSlice:0 destinationLevel:0 sliceCount:1 levelCount:1];
        [bl endEncoding];
    }
    src->used = dst->used = M.serial;
    return 0;
}

/* ---- shaders: HLSL -> SPIR-V -> MSL ------------------------------------------------------------------- */

static void semantic_of(const char *name, char *sem, size_t n, uint32_t *index)
{
    const char *dot = strrchr(name, '.');
    const char *p = dot ? dot + 1 : name;
    size_t len = strlen(p), k;

    while (len && isdigit((unsigned char)p[len - 1]))
        len--;
    *index = (uint32_t)atoi(p + len);
    if (len >= n)
        len = n - 1;
    for (k = 0; k < len; k++)
        sem[k] = (char)toupper((unsigned char)p[k]);
    sem[len] = 0;
}

/* One location per semantic, the same in every shader, so any vertex shader
 * links with any pixel shader without either being compiled for the other. */
static uint32_t canonical_location(const char *sem, uint32_t idx)
{
    uint32_t h = 2166136261u;
    const char *c;

    if (!strcmp(sem, "COLOR") && idx < 2)     return idx;
    if (!strcmp(sem, "TEXCOORD") && idx < 16) return 2 + idx;
    if (!strcmp(sem, "FOG"))                  return 18;
    if (!strcmp(sem, "PSIZE"))                return 19;
    for (c = sem; *c; c++)
        h = (h ^ (unsigned char)*c) * 16777619u;
    return 20 + (h + idx) % 10;
}

typedef struct { char *p; size_t n, cap; } Str;

static void str_add(Str *s, const char *t, size_t n)
{
    if (s->n + n + 1 > s->cap) {
        size_t cap = (s->cap ? s->cap * 2 : 4096);
        while (cap < s->n + n + 1) cap *= 2;
        s->p = realloc(s->p, cap);
        s->cap = cap;
    }
    memcpy(s->p + s->n, t, n);
    s->n += n;
    s->p[s->n] = 0;
}

/* The vertex side's varyings as float4: Metal refuses a float output linked
 * to a float4 input, which D3D reads as .x. SPIRV-Cross writes each output
 * member on its own line and assigns it whole, so a declaration and its
 * assignments are the only places the type shows. */
static char *widen_outputs(const char *msl, const char *entry)
{
    char head[192];
    const char *st, *end, *line;
    Str out = { 0 };
    struct { char name[96]; int comps; } w[32];
    int nw = 0, i;

    snprintf(head, sizeof head, "struct %s_out\n{\n", entry);
    st = strstr(msl, head);
    if (!st)
        return strdup(msl);
    end = strstr(st, "};");
    if (!end)
        return strdup(msl);
    /* the declarations */
    str_add(&out, msl, (size_t)(st - msl));
    for (line = st; line < end; ) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line + 1) : strlen(line);
        int comps = 0;
        char name[96];
        const char *user = strstr(line, " [[user(");
        const char *nm = NULL;
        /* exactly "    float NAME [[user(", "    float2 ..." or "    float3 ...":
         * user varyings only, never a builtin such as [[point_size]] */
        if (user && user < line + len) {
            if (!strncmp(line, "    float ", 10))       { comps = 1; nm = line + 10; }
            else if (!strncmp(line, "    float2 ", 11))  { comps = 2; nm = line + 11; }
            else if (!strncmp(line, "    float3 ", 11))  { comps = 3; nm = line + 11; }
        }
        if (comps && (size_t)(user - nm) < sizeof name && user > nm) {
            memcpy(name, nm, (size_t)(user - nm));
            name[user - nm] = 0;
        } else {
            comps = 0;
        }
        if (comps && nw < 32) {
            const char *rest = user;
            snprintf(w[nw].name, sizeof w[nw].name, "%s", name);
            w[nw].comps = comps;
            nw++;
            str_add(&out, "    float4 ", 11);
            str_add(&out, name, strlen(name));
            str_add(&out, rest, (size_t)(line + len - rest));
        } else {
            str_add(&out, line, len);
        }
        line += len;
    }
    /* the assignments: out.NAME = expr;  ->  out.NAME = float4(expr, 0...); */
    for (line = end; *line; ) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line + 1) : strlen(line);
        int done = 0;
        for (i = 0; i < nw && !done; i++) {
            char pat[128];
            const char *at, *semi;
            snprintf(pat, sizeof pat, "out.%s = ", w[i].name);
            at = strstr(line, pat);
            if (at && at < line + len && (semi = memchr(at, ';', (size_t)(line + len - at)))) {
                const char *expr = at + strlen(pat);
                static const char *pads[] = { "", ", 0.0, 0.0, 0.0", ", 0.0, 0.0", ", 0.0" };
                str_add(&out, line, (size_t)(expr - line));
                str_add(&out, "float4(", 7);
                str_add(&out, expr, (size_t)(semi - expr));
                str_add(&out, pads[w[i].comps], strlen(pads[w[i].comps]));
                str_add(&out, ")", 1);
                str_add(&out, semi, (size_t)(line + len - semi));
                done = 1;
            }
        }
        if (!done)
            str_add(&out, line, len);
        line += len;
    }
    return out.p ? out.p : strdup(msl);
}

/* RECOMP_METAL_DUMP=<dir>: every MSL source as it goes to Metal. */
static void dump_msl(const char *msl, const char *entry)
{
    static unsigned n;
    const char *dir = getenv("RECOMP_METAL_DUMP");
    char path[1024];
    FILE *f;
    if (!dir || !*dir)
        return;
    snprintf(path, sizeof path, "%s/%04u_%s.metal", dir, n++, entry);
    if ((f = fopen(path, "w"))) {
        fputs(msl, f);
        fclose(f);
    }
}

static id<MTLFunction> compile_msl(const char *msl, const char *entry, char *err, size_t err_len)
{
    id<MTLFunction> fn = nil;
    dump_msl(msl, entry);
    @autoreleasepool {
        NSError *e = nil;
        MTLCompileOptions *o = [[MTLCompileOptions alloc] init];
        id<MTLLibrary> lib;
        o.languageVersion = MTLLanguageVersion2_3;
        lib = [M.dev newLibraryWithSource:[NSString stringWithUTF8String:msl] options:o error:&e];
        [o release];
        if (lib) {
            fn = [lib newFunctionWithName:[NSString stringWithUTF8String:entry]];
            [lib release];
        }
        if (!fn && err && err_len)
            snprintf(err, err_len, "Metal: %s", e ? e.localizedDescription.UTF8String : "no function");
    }
    return fn;
}

static int to_msl(RhiShader *s, const uint32_t *spirv, size_t words, const char *entry,
                  char *err, size_t err_len)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir;
    spvc_compiler comp;
    spvc_compiler_options opt;
    spvc_resources res;
    const spvc_reflected_resource *list;
    size_t count, i;
    const char *msl = NULL, *clean;
    int vs = s->stage == RHI_STAGE_VERTEX, ok = 0;
    SpvExecutionModel model = vs ? SpvExecutionModelVertex : SpvExecutionModelFragment;
    uint32_t b;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS)
        return 0;
    if (spvc_context_parse_spirv(ctx, spirv, words, &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                     &comp) != SPVC_SUCCESS)
        goto out;
    spvc_compiler_create_compiler_options(comp, &opt);
    spvc_compiler_options_set_uint(opt, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 3, 0));
    spvc_compiler_options_set_uint(opt, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS);
    spvc_compiler_install_compiler_options(comp, opt);

    for (b = 0; b < 48; b++) {
        spvc_msl_resource_binding_2 rb;
        spvc_msl_resource_binding_init_2(&rb);
        rb.stage = model;
        rb.desc_set = 0;
        rb.binding = b;
        rb.count = 1;
        if (b < RHI_VK_TEXTURE_BASE)
            rb.msl_buffer = b % RHI_VK_PS_UNIFORM_BASE;
        else if (b < RHI_VK_SAMPLER_BASE)
            rb.msl_texture = b - RHI_VK_TEXTURE_BASE;
        else
            rb.msl_sampler = b - RHI_VK_SAMPLER_BASE;
        spvc_compiler_msl_add_resource_binding_2(comp, &rb);
    }
    if (spvc_compiler_create_shader_resources(comp, &res) != SPVC_SUCCESS)
        goto out;

    /* varyings at their canonical locations */
    spvc_resources_get_resource_list_for_type(res, vs ? SPVC_RESOURCE_TYPE_STAGE_OUTPUT
                                                      : SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &count);
    for (i = 0; i < count; i++) {
        char sem[40];
        uint32_t idx, loc;
        spvc_msl_shader_interface_var_2 v;
        semantic_of(list[i].name, sem, sizeof sem, &idx);
        loc = canonical_location(sem, idx);
        spvc_compiler_set_decoration(comp, list[i].id, SpvDecorationLocation, loc);
        spvc_msl_shader_interface_var_init_2(&v);
        v.location = loc;
        v.vecsize = 4;
        if (vs)
            spvc_compiler_msl_add_shader_output_2(comp, &v);
        else
            spvc_compiler_msl_add_shader_input_2(comp, &v);
        s->io_mask |= 1u << loc;
    }
    /* vertex inputs: semantic -> attribute */
    if (vs) {
        spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &count);
        for (i = 0; i < count && s->nin < 32; i++) {
            IoVar *io = &s->in[s->nin++];
            semantic_of(list[i].name, io->sem, sizeof io->sem, &io->index);
            io->loc = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationLocation);
        }
    }
    /* constant buffers, textures, samplers */
    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count);
    for (i = 0; i < count; i++)
        s->ubo_mask |= 1u << (spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding) %
                              RHI_VK_PS_UNIFORM_BASE);
    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &list, &count);
    for (i = 0; i < count; i++) {
        uint32_t slot = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding) -
                        RHI_VK_TEXTURE_BASE;
        spvc_type t = spvc_compiler_get_type_handle(comp, list[i].type_id);
        SpvDim dim = spvc_type_get_image_dimension(t);
        if (slot >= 16)
            continue;
        s->tex_mask |= 1u << slot;
        s->tex_kind[slot] = dim == SpvDim3D ? TEX_3D : dim == SpvDimCube ? TEX_CUBE :
                            spvc_type_get_image_multisampled(t) ? TEX_2D_MS :
                            spvc_type_get_image_arrayed(t) ? TEX_2D_ARRAY : TEX_2D;
    }
    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS, &list, &count);
    for (i = 0; i < count; i++) {
        uint32_t slot = spvc_compiler_get_decoration(comp, list[i].id, SpvDecorationBinding) -
                        RHI_VK_SAMPLER_BASE;
        if (slot < 16)
            s->smp_mask |= 1u << slot;
    }

    if (spvc_compiler_compile(comp, &msl) != SPVC_SUCCESS || !msl) {
        if (err && err_len)
            snprintf(err, err_len, "SPIRV-Cross: %s", spvc_context_get_last_error_string(ctx));
        goto out;
    }
    clean = spvc_compiler_get_cleansed_entry_point_name(comp, entry, model);
    snprintf(s->entry, sizeof s->entry, "%s", clean ? clean : entry);
    s->msl = vs ? widen_outputs(msl, s->entry) : strdup(msl);
    ok = s->msl != NULL;
out:
    if (!ok && err && err_len && !err[0])
        snprintf(err, err_len, "SPIRV-Cross: %s", spvc_context_get_last_error_string(ctx));
    spvc_context_destroy(ctx);
    return ok;
}

static RhiShader *m_shader_create(uint32_t stage, const RhiShaderSource *src, char *err, size_t err_len)
{
    RhiShader *s;
    uint32_t *code = NULL;
    size_t bytes = 0;

    if (err && err_len)
        err[0] = 0;
    d3d8_hlsl_note(src->hlsl, src->len, src->name, src->macros, src->entry, src->target);
    if (!rhi_shader_cache_get(src, MTL_SPIRV_TAG, (void **)&code, &bytes) || bytes < 20 || (bytes & 3)) {
        free(code);
        code = NULL;
        if (!rhi_vk_dxc_compile(stage, src, &code, &bytes, err, err_len))
            return NULL;
        rhi_shader_cache_put(src, MTL_SPIRV_TAG, code, bytes);
    }
    s = calloc(1, sizeof *s);
    if (!s) {
        free(code);
        return NULL;
    }
    s->stage = stage;
    s->id = ++M.next_id;
    if (!to_msl(s, code, bytes / 4, src->entry ? src->entry : "main", err, err_len)) {
        free(code);
        free(s->msl);
        free(s);
        return NULL;
    }
    free(code);
    s->fn = compile_msl(s->msl, s->entry, err, err_len);
    if (!s->fn) {
        static int said;
        if (said++ < 5)
            fprintf(stderr, "[RHI] metal: %s shader %s did not compile: %s\n",
                    stage == RHI_STAGE_VERTEX ? "vertex" : "pixel", src->name ? src->name : "",
                    err ? err : "");
        free(s->msl);
        free(s);
        return NULL;
    }
    if (stage != RHI_STAGE_VERTEX) {
        free(s->msl);
        s->msl = NULL;
    }
    return s;
}

static void m_shader_destroy(RhiShader *s)
{
    uint32_t i;
    if (!s)
        return;
    if (M.vs == s) M.vs = NULL;
    if (M.ps == s) M.ps = NULL;
    for (i = 0; i < s->nvariants; i++)
        [s->variants[i].fn release];
    [s->fn release];
    free(s->msl);
    free(s);
}

/* A vertex shader that also writes zero at the locations a pixel shader
 * reads and it does not: Metal refuses the pair otherwise. */
static id<MTLFunction> pad_variant(RhiShader *vs, uint32_t missing)
{
    char head[192], *src, *at;
    Str add = { 0 }, all = { 0 };
    uint32_t i;
    id<MTLFunction> fn;

    for (i = 0; i < vs->nvariants; i++)
        if (vs->variants[i].mask == missing)
            return vs->variants[i].fn;
    if (!vs->msl || vs->nvariants >= 8)
        return nil;
    snprintf(head, sizeof head, "struct %s_out\n{\n", vs->entry);
    src = vs->msl;
    at = strstr(src, head);
    if (!at)
        return nil;
    for (i = 0; i < 32; i++)
        if (missing & (1u << i)) {
            char line[96];
            int n = snprintf(line, sizeof line, "    float4 rhi_pad%u [[user(locn%u)]];\n", i, i);
            str_add(&add, line, (size_t)n);
        }
    at += strlen(head);
    str_add(&all, src, (size_t)(at - src));
    str_add(&all, add.p, add.n);
    str_add(&all, at, strlen(at));
    fn = compile_msl(all.p, vs->entry, NULL, 0);
    free(add.p);
    free(all.p);
    if (fn) {
        vs->variants[vs->nvariants].mask = missing;
        vs->variants[vs->nvariants].fn = fn;
        vs->nvariants++;
    }
    return fn;
}

static RhiVertexLayout *m_vertex_layout_create(const RhiVertexElement *e, uint32_t n, const RhiShader *vs)
{
    RhiVertexLayout *l;
    uint32_t i;

    (void)vs;
    if (n > 32)
        return NULL;
    l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->id = ++M.next_id;
    l->n = n;
    for (i = 0; i < n; i++) {
        l->e[i] = e[i];
        snprintf(l->names[i], sizeof l->names[i], "%s", e[i].semantic ? e[i].semantic : "");
        l->e[i].semantic = l->names[i];
    }
    return l;
}

static void m_vertex_layout_destroy(RhiVertexLayout *l)
{
    if (M.layout == l)
        M.layout = NULL;
    free(l);
}

/* ---- state objects ----------------------------------------------------------------------------------- */

static MTLCompareFunction mtl_cmp(uint32_t c)       { return c ? (MTLCompareFunction)(c - 1) : MTLCompareFunctionAlways; }
static MTLStencilOperation mtl_stencil(uint32_t o)  { return o ? (MTLStencilOperation)(o - 1) : MTLStencilOperationKeep; }

static RhiBlendState *m_blend_state_create(const RhiBlendDesc *d)
{
    RhiBlendState *s = calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

static RhiRasterState *m_raster_state_create(const RhiRasterDesc *d)
{
    RhiRasterState *s = calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

static void stencil_face(MTLStencilDescriptor *sd, const RhiStencilFace *f, uint8_t rm, uint8_t wm)
{
    sd.stencilCompareFunction = mtl_cmp(f->func);
    sd.stencilFailureOperation = mtl_stencil(f->fail);
    sd.depthFailureOperation = mtl_stencil(f->depth_fail);
    sd.depthStencilPassOperation = mtl_stencil(f->pass);
    sd.readMask = rm;
    sd.writeMask = wm;
}

static RhiDepthState *m_depth_state_create(const RhiDepthDesc *d)
{
    RhiDepthState *s = calloc(1, sizeof *s);
    MTLDepthStencilDescriptor *dd;

    if (!s)
        return NULL;
    s->d = *d;
    dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = d->depth_enable ? mtl_cmp(d->depth_func) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = d->depth_enable && d->depth_write;
    if (d->stencil_enable) {
        MTLStencilDescriptor *f = [[MTLStencilDescriptor alloc] init];
        MTLStencilDescriptor *b = [[MTLStencilDescriptor alloc] init];
        stencil_face(f, &d->front, d->stencil_read_mask, d->stencil_write_mask);
        stencil_face(b, &d->back, d->stencil_read_mask, d->stencil_write_mask);
        dd.frontFaceStencil = f;
        dd.backFaceStencil = b;
        [f release];
        [b release];
    }
    s->dss = [M.dev newDepthStencilStateWithDescriptor:dd];
    [dd release];
    return s;
}

static MTLSamplerAddressMode mtl_address(uint32_t a)
{
    switch (a) {
    case RHI_ADDRESS_MIRROR:      return MTLSamplerAddressModeMirrorRepeat;
    case RHI_ADDRESS_CLAMP:       return MTLSamplerAddressModeClampToEdge;
    case RHI_ADDRESS_BORDER:      return MTLSamplerAddressModeClampToBorderColor;
    case RHI_ADDRESS_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
    default:                      return MTLSamplerAddressModeRepeat;
    }
}

static RhiSampler *m_sampler_create(const RhiSamplerDesc *d)
{
    RhiSampler *s = calloc(1, sizeof *s);
    MTLSamplerDescriptor *sd;
    static int said_bias;

    if (!s)
        return NULL;
    s->d = *d;
    sd = [[MTLSamplerDescriptor alloc] init];
    if ((d->filter & 0x55) == 0x55 && d->max_anisotropy > 1) {
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.maxAnisotropy = d->max_anisotropy > 16 ? 16 : d->max_anisotropy;
    } else {
        sd.minFilter = (d->filter & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = (d->filter & 0x04) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = (d->filter & 0x01) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    }
    sd.sAddressMode = mtl_address(d->address_u);
    sd.tAddressMode = mtl_address(d->address_v);
    sd.rAddressMode = mtl_address(d->address_w);
    if (d->address_u == RHI_ADDRESS_BORDER || d->address_v == RHI_ADDRESS_BORDER ||
        d->address_w == RHI_ADDRESS_BORDER) {
        /* Metal has three border colours: the nearest of them */
        if (d->border[3] < 0.5f)
            sd.borderColor = MTLSamplerBorderColorTransparentBlack;
        else if (d->border[0] + d->border[1] + d->border[2] > 1.5f)
            sd.borderColor = MTLSamplerBorderColorOpaqueWhite;
        else
            sd.borderColor = MTLSamplerBorderColorOpaqueBlack;
    }
    sd.lodMinClamp = d->min_lod > 0 ? d->min_lod : 0;
    sd.lodMaxClamp = d->max_lod < 1000.0f ? d->max_lod : FLT_MAX;
    if (d->filter & 0x80)
        sd.compareFunction = mtl_cmp(d->compare);
    if (d->mip_lod_bias != 0.0f)
        say_once(&said_bias, "a mip LOD bias is ignored (Metal samplers have none)");
    s->s = [M.dev newSamplerStateWithDescriptor:sd];
    [sd release];
    return s;
}

static void m_blend_state_destroy(RhiBlendState *s)   { if (M.blend == s) M.blend = NULL; free(s); }
static void m_raster_state_destroy(RhiRasterState *s) { if (M.raster == s) M.raster = NULL; free(s); }
static void m_depth_state_destroy(RhiDepthState *s)
{
    if (!s) return;
    if (M.depth == s) M.depth = NULL;
    [s->dss release];
    free(s);
}
static void m_sampler_destroy(RhiSampler *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < 16; i++)
        if (M.smp[i] == s)
            M.smp[i] = NULL;
    [s->s release];
    free(s);
}

/* ---- pipelines --------------------------------------------------------------------------------------- */

static MTLBlendFactor mtl_blend(uint32_t b)
{
    switch (b) {
    case RHI_BLEND_ZERO:           return MTLBlendFactorZero;
    case RHI_BLEND_ONE:            return MTLBlendFactorOne;
    case RHI_BLEND_SRC_COLOR:      return MTLBlendFactorSourceColor;
    case RHI_BLEND_INV_SRC_COLOR:  return MTLBlendFactorOneMinusSourceColor;
    case RHI_BLEND_SRC_ALPHA:      return MTLBlendFactorSourceAlpha;
    case RHI_BLEND_INV_SRC_ALPHA:  return MTLBlendFactorOneMinusSourceAlpha;
    case RHI_BLEND_DEST_ALPHA:     return MTLBlendFactorDestinationAlpha;
    case RHI_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case RHI_BLEND_DEST_COLOR:     return MTLBlendFactorDestinationColor;
    case RHI_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case RHI_BLEND_SRC_ALPHA_SAT:  return MTLBlendFactorSourceAlphaSaturated;
    case RHI_BLEND_FACTOR:         return MTLBlendFactorBlendColor;
    case RHI_BLEND_INV_FACTOR:     return MTLBlendFactorOneMinusBlendColor;
    default:                       return MTLBlendFactorOne;
    }
}

static MTLBlendOperation mtl_blend_op(uint32_t op)
{
    switch (op) {
    case RHI_BLEND_OP_SUBTRACT:     return MTLBlendOperationSubtract;
    case RHI_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case RHI_BLEND_OP_MIN:          return MTLBlendOperationMin;
    case RHI_BLEND_OP_MAX:          return MTLBlendOperationMax;
    default:                        return MTLBlendOperationAdd;
    }
}

/* D3D's write mask is R=1 G=2 B=4 A=8; Metal's runs the other way. */
static MTLColorWriteMask mtl_write_mask(uint32_t m)
{
    MTLColorWriteMask r = MTLColorWriteMaskNone;
    if (m & RHI_WRITE_R) r |= MTLColorWriteMaskRed;
    if (m & RHI_WRITE_G) r |= MTLColorWriteMaskGreen;
    if (m & RHI_WRITE_B) r |= MTLColorWriteMaskBlue;
    if (m & RHI_WRITE_A) r |= MTLColorWriteMaskAlpha;
    return r;
}

static int same_semantic(const IoVar *a, const char *sem, uint32_t index)
{
    return a->index == index && strcmp(a->sem, sem) == 0;
}

static uint64_t hash_key(const PipeKey *k)
{
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < sizeof *k; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static Pipe *build_pipeline(const PipeKey *key)
{
    Pipe *p = calloc(1, sizeof *p);
    const RhiShader *vs = M.vs, *ps = M.ps;
    const RhiVertexLayout *l = M.layout;
    uint32_t i, k, slot_off[MAX_VB];
    id<MTLFunction> vfn = vs->fn;

    if (!p)
        return NULL;
    p->key = *key;
    if (ps) {
        uint32_t missing = ps->io_mask & ~vs->io_mask;
        if (missing) {
            vfn = pad_variant((RhiShader *)vs, missing);
            if (!vfn) {
                fprintf(stderr, "[RHI] metal: no vertex variant for pixel inputs 0x%x\n", missing);
                return p;
            }
        }
    }
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        NSError *err = nil;
        int zero_used = 0;

        pd.vertexFunction = vfn;
        pd.fragmentFunction = ps ? ps->fn : nil;
        memset(slot_off, 0, sizeof slot_off);
        /* every input the vertex shader reads: from the layout, or zero */
        for (k = 0; k < vs->nin; k++) {
            const IoVar *in = &vs->in[k];
            int found = 0;
            memset(slot_off, 0, sizeof slot_off);
            for (i = 0; l && i < l->n; i++) {
                const RhiVertexElement *e = &l->e[i];
                char sem[40];
                size_t n = strlen(e->semantic), j;
                uint32_t off;
                if (e->slot >= MAX_VB)
                    continue;
                off = e->offset == RHI_APPEND ? slot_off[e->slot] : e->offset;
                slot_off[e->slot] = off + rhi_format_row_pitch(e->format, 1);
                if (n >= sizeof sem) n = sizeof sem - 1;
                for (j = 0; j < n; j++)
                    sem[j] = (char)toupper((unsigned char)e->semantic[j]);
                sem[n] = 0;
                if (!same_semantic(in, sem, e->semantic_index))
                    continue;
                vd.attributes[in->loc].format = mtl_vertex_format(e->format);
                vd.attributes[in->loc].offset = off;
                vd.attributes[in->loc].bufferIndex = VB_BASE + e->slot;
                if (key->strides[e->slot]) {
                    vd.layouts[VB_BASE + e->slot].stride = key->strides[e->slot];
                    vd.layouts[VB_BASE + e->slot].stepFunction = MTLVertexStepFunctionPerVertex;
                } else {
                    vd.layouts[VB_BASE + e->slot].stride = 16;
                    vd.layouts[VB_BASE + e->slot].stepFunction = MTLVertexStepFunctionConstant;
                    vd.layouts[VB_BASE + e->slot].stepRate = 0;
                }
                p->vb_used |= (uint16_t)(1u << e->slot);
                found = 1;
                break;
            }
            if (!found) {
                vd.attributes[in->loc].format = MTLVertexFormatFloat4;
                vd.attributes[in->loc].offset = 0;
                vd.attributes[in->loc].bufferIndex = ZERO_VB;
                zero_used = 1;
            }
        }
        if (zero_used) {
            vd.layouts[ZERO_VB].stride = 16;
            vd.layouts[ZERO_VB].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[ZERO_VB].stepRate = 0;
        }
        pd.vertexDescriptor = vd;
        if (key->color != MTLPixelFormatInvalid) {
            MTLRenderPipelineColorAttachmentDescriptor *c = pd.colorAttachments[0];
            c.pixelFormat = key->color;
            c.blendingEnabled = key->blend_enable ? YES : NO;
            c.sourceRGBBlendFactor = mtl_blend(key->src);
            c.destinationRGBBlendFactor = mtl_blend(key->dst);
            c.rgbBlendOperation = mtl_blend_op(key->op);
            c.sourceAlphaBlendFactor = mtl_blend(key->src_a);
            c.destinationAlphaBlendFactor = mtl_blend(key->dst_a);
            c.alphaBlendOperation = mtl_blend_op(key->op_a);
            /* no pixel shader writes no colour, as under D3D11 */
            c.writeMask = ps ? mtl_write_mask(key->write_mask) : MTLColorWriteMaskNone;
        }
        pd.depthAttachmentPixelFormat = key->depth;
        if (has_stencil_pf(key->depth))
            pd.stencilAttachmentPixelFormat = key->depth;
        pd.rasterSampleCount = key->samples ? key->samples : 1;
        pd.alphaToCoverageEnabled = key->a2c ? YES : NO;
        p->state = [M.dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p->state) {
            static int said;
            if (said++ < 10)
                fprintf(stderr, "[RHI] metal: a pipeline could not be built; its draws are skipped: %s\n",
                        err ? err.localizedDescription.UTF8String : "?");
        }
        [pd release];
    }
    return p;
}

static Pipe *find_pipeline(void)
{
    PipeKey key;
    const RhiBlendDesc *b = M.blend ? &M.blend->d : NULL;
    uint64_t h;
    Pipe *p;
    uint32_t i;

    memset(&key, 0, sizeof key);
    key.vs = M.vs->id;
    key.ps = M.ps ? M.ps->id : 0;
    key.layout = M.layout ? M.layout->id : 0;
    for (i = 0; i < MAX_VB; i++)
        key.strides[i] = M.vb[i].b ? M.vb[i].stride : 0;
    key.color = M.e.has_color ? M.e.color_pf : MTLPixelFormatInvalid;
    key.depth = M.e.has_depth ? M.e.depth_pf : MTLPixelFormatInvalid;
    key.samples = M.e.samples;
    if (b) {
        key.blend_enable = b->enable;
        key.src = b->src; key.dst = b->dst; key.op = b->op;
        key.src_a = b->src_alpha; key.dst_a = b->dst_alpha; key.op_a = b->op_alpha;
        key.write_mask = b->write_mask;
        key.a2c = b->alpha_to_coverage;
    } else {
        key.write_mask = RHI_WRITE_ALL;
    }
    h = hash_key(&key);
    for (p = M.pipes[h & 1023]; p; p = p->next)
        if (memcmp(&p->key, &key, sizeof key) == 0)
            return p;
    p = build_pipeline(&key);
    M.st.pipelines++;
    if (p) {
        p->next = M.pipes[h & 1023];
        M.pipes[h & 1023] = p;
    }
    return p;
}

/* ---- drawing ----------------------------------------------------------------------------------------- */

static RhiView *dummy_for(uint32_t kind)
{
    return M.dummy[kind < 5 ? kind : 0];
}

static int view_fits(const RhiView *v, uint32_t kind)
{
    MTLTextureType t = v->sampled.textureType;
    switch (kind) {
    case TEX_2D:       return t == MTLTextureType2D;
    case TEX_2D_ARRAY: return t == MTLTextureType2DArray;
    case TEX_CUBE:     return t == MTLTextureTypeCube;
    case TEX_3D:       return t == MTLTextureType3D;
    case TEX_2D_MS:    return t == MTLTextureType2DMultisample;
    }
    return 0;
}

static void bind_vertex_buffer(uint32_t index, id<MTLBuffer> buf, uint32_t off)
{
    if (M.e.vbuf[index] == buf) {
        if (M.e.voff[index] != off) {
            [M.enc setVertexBufferOffset:off atIndex:index];
            M.e.voff[index] = off;
        }
        return;
    }
    [M.enc setVertexBuffer:buf offset:off atIndex:index];
    M.e.vbuf[index] = buf;
    M.e.voff[index] = off;
}

static void bind_fragment_buffer(uint32_t index, id<MTLBuffer> buf, uint32_t off)
{
    if (M.e.fbuf[index] == buf) {
        if (M.e.foff[index] != off) {
            [M.enc setFragmentBufferOffset:off atIndex:index];
            M.e.foff[index] = off;
        }
        return;
    }
    [M.enc setFragmentBuffer:buf offset:off atIndex:index];
    M.e.fbuf[index] = buf;
    M.e.foff[index] = off;
}

/* A constant buffer for one stage: its bytes, sent again only when they or
 * the buffer changed since this encoder last had them. A larger one is an
 * ordinary buffer binding. */
static void bind_constants(int stage, uint32_t slot, RhiBuffer *b)
{
    if (!b) {
        if (stage == 0) bind_vertex_buffer(slot, M.zero, 0);
        else bind_fragment_buffer(slot, M.zero, 0);
        M.e.cb[stage][slot] = NULL;
        return;
    }
    if (b->bytes) {
        uint32_t size = (b->d.size + 15u) & ~15u;
        if (M.e.cb[stage][slot] == b && M.e.cb_gen[stage][slot] == b->gen)
            return;
        if (size > BYTES_MAX) size = BYTES_MAX;
        if (stage == 0) {
            [M.enc setVertexBytes:b->bytes length:size atIndex:slot];
            M.e.vbuf[slot] = nil;
        } else {
            [M.enc setFragmentBytes:b->bytes length:size atIndex:slot];
            M.e.fbuf[slot] = nil;
        }
        M.e.cb[stage][slot] = b;
        M.e.cb_gen[stage][slot] = b->gen;
        return;
    }
    M.e.cb[stage][slot] = NULL;
    if (stage == 0) bind_vertex_buffer(slot, buffer_now(b), 0);
    else bind_fragment_buffer(slot, buffer_now(b), 0);
}

static int prepare_draw(void)
{
    Pipe *p;
    uint32_t i;
    const RhiRasterDesc *r = M.raster ? &M.raster->d : NULL;
    id<MTLDepthStencilState> dss;

    if (!M.vs || !M.vs->fn)
        return 0;
    /* textures about to be sampled that still owe a clear */
    for (i = 0; M.ps && i < 16; i++)
        if ((M.ps->tex_mask & (1u << i)) && M.tex[i] &&
            (M.tex[i]->img->clear_color || M.tex[i]->img->clear_ds))
            flush_clear(M.tex[i]->img);
    if (!begin_pass())
        return 0;
    p = find_pipeline();
    if (!p || !p->state)
        return 0;
    if (M.e.pipe != p->state) {
        [M.enc setRenderPipelineState:p->state];
        M.e.pipe = p->state;
    }
    dss = M.e.has_depth && M.depth ? M.depth->dss : M.no_depth;
    if (M.e.dss != dss) {
        [M.enc setDepthStencilState:dss];
        M.e.dss = dss;
    }
    [M.enc setStencilReferenceValue:M.stencil_ref];
    [M.enc setBlendColorRed:M.blend_factor[0] green:M.blend_factor[1] blue:M.blend_factor[2]
                      alpha:M.blend_factor[3]];
    if (r) {
        [M.enc setCullMode:r->cull == RHI_CULL_FRONT ? MTLCullModeFront :
                           r->cull == RHI_CULL_BACK ? MTLCullModeBack : MTLCullModeNone];
        [M.enc setFrontFacingWinding:r->front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
        [M.enc setDepthBias:(float)r->depth_bias slopeScale:r->slope_scaled_depth_bias
                      clamp:r->depth_bias_clamp];
        [M.enc setDepthClipMode:r->depth_clip ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
        [M.enc setTriangleFillMode:r->fill == RHI_FILL_WIREFRAME ? MTLTriangleFillModeLines
                                                                 : MTLTriangleFillModeFill];
    } else {
        [M.enc setCullMode:MTLCullModeBack];
        [M.enc setFrontFacingWinding:MTLWindingClockwise];
    }
    {
        MTLViewport vp = { M.vp.x, M.vp.y, M.vp.width, M.vp.height, M.vp.min_depth, M.vp.max_depth };
        MTLScissorRect sc = { 0, 0, M.e.width, M.e.height };
        if (vp.width <= 0 || vp.height <= 0) {
            vp.originX = vp.originY = 0;
            vp.width = M.e.width;
            vp.height = M.e.height;
            vp.znear = 0;
            vp.zfar = 1;
        }
        [M.enc setViewport:vp];
        if (r && r->scissor) {
            int32_t x0 = M.scissor.left < 0 ? 0 : M.scissor.left;
            int32_t y0 = M.scissor.top < 0 ? 0 : M.scissor.top;
            int32_t x1 = M.scissor.right > (int32_t)M.e.width ? (int32_t)M.e.width : M.scissor.right;
            int32_t y1 = M.scissor.bottom > (int32_t)M.e.height ? (int32_t)M.e.height : M.scissor.bottom;
            if (x1 <= x0 || y1 <= y0)
                return 0;               /* nothing inside: D3D draws nothing either */
            sc.x = (NSUInteger)x0;
            sc.y = (NSUInteger)y0;
            sc.width = (NSUInteger)(x1 - x0);
            sc.height = (NSUInteger)(y1 - y0);
        }
        [M.enc setScissorRect:sc];
    }
    /* vertex streams */
    for (i = 0; i < MAX_VB; i++)
        if (p->vb_used & (1u << i)) {
            RhiBuffer *b = M.vb[i].b;
            if (b) {
                if (b->bytes)
                    continue;           /* a constant buffer bound as a stream: nothing to read */
                bind_vertex_buffer(VB_BASE + i, buffer_now(b), M.vb[i].offset);
            } else {
                bind_vertex_buffer(VB_BASE + i, M.zero, 0);
            }
        }
    bind_vertex_buffer(ZERO_VB, M.zero, 0);
    /* constant buffers */
    for (i = 0; i < 8; i++)
        if (M.vs->ubo_mask & (1u << i))
            bind_constants(0, i, M.ubo[0][i]);
    if (M.ps) {
        for (i = 0; i < 8; i++)
            if (M.ps->ubo_mask & (1u << i))
                bind_constants(1, i, M.ubo[1][i]);
        for (i = 0; i < 16; i++) {
            if (M.ps->tex_mask & (1u << i)) {
                RhiView *v = M.tex[i];
                id<MTLTexture> t;
                if (!v || !v->sampled || !view_fits(v, M.ps->tex_kind[i]))
                    v = dummy_for(M.ps->tex_kind[i]);
                t = v ? v->sampled : nil;
                if (v) v->img->used = M.serial;
                if (M.e.tex[i] != t) {
                    [M.enc setFragmentTexture:t atIndex:i];
                    M.e.tex[i] = t;
                }
            }
            if (M.ps->smp_mask & (1u << i)) {
                id<MTLSamplerState> s = M.smp[i] ? M.smp[i]->s : M.def_sampler;
                if (M.e.smp[i] != s) {
                    [M.enc setFragmentSamplerState:s atIndex:i];
                    M.e.smp[i] = s;
                }
            }
        }
    }
    return 1;
}

static MTLPrimitiveType mtl_topology(uint32_t t)
{
    switch (t) {
    case RHI_TOPOLOGY_POINTS:         return MTLPrimitiveTypePoint;
    case RHI_TOPOLOGY_LINES:          return MTLPrimitiveTypeLine;
    case RHI_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
    case RHI_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default:                          return MTLPrimitiveTypeTriangle;
    }
}

static void occl_make_room(void);
static int occl_slot_begin(void);
static void occl_slot_end(int slot);

static void m_draw(uint32_t n, uint32_t first)
{
    if (!n)
        return;
    @autoreleasepool {
        int slot;
        occl_make_room();
        if (!prepare_draw())
            return;
        slot = occl_slot_begin();
        [M.enc drawPrimitives:mtl_topology(M.topology) vertexStart:first vertexCount:n];
        occl_slot_end(slot);
    }
}

static void m_draw_indexed(uint32_t n, uint32_t first, int32_t base)
{
    uint32_t isz = M.ib_bits == 32 ? 4 : 2;
    id<MTLBuffer> buf;
    uint32_t off;

    if (!n || !M.ib)
        return;
    @autoreleasepool {
        int slot;
        occl_make_room();
        if (!prepare_draw())
            return;
        if (M.ib->bytes)
            return;
        buf = buffer_now(M.ib);
        off = M.ib_off + first * isz;
        if (off & 3) {
            /* Metal wants the index offset 4-byte aligned: a copy that is */
            uint8_t *p = ring_alloc(n * isz, 4, &buf, &off);
            if (!p)
                return;
            memcpy(p, (const uint8_t *)[M.ib->v[M.ib->cur].buf contents] + M.ib_off + first * isz, n * isz);
        }
        slot = occl_slot_begin();
        [M.enc drawIndexedPrimitives:mtl_topology(M.topology) indexCount:n
                           indexType:isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                         indexBuffer:buf indexBufferOffset:off instanceCount:1
                          baseVertex:base baseInstance:0];
        occl_slot_end(slot);
    }
}

/* ---- occlusion counting ------------------------------------------------------------------------------ */

/* Before a counted draw opens its pass: when the slots run out, every count
 * made with them must be finished before one is reused. Doing it here keeps
 * the wait (which ends the encoder) out of the middle of a draw. */
static void occl_make_room(void)
{
    if (M.occl_on && M.occl && M.occl_next >= OCCL_SLOTS) {
        wait_serial(M.occl_serial);
        M.occl_next = 0;
        M.occl_gen++;
    }
}

static int occl_slot_begin(void)
{
    uint32_t slot;
    if (!M.occl_on || !M.occl || !M.enc || M.occl_next >= OCCL_SLOTS)
        return -1;
    slot = M.occl_next++;
    ((uint64_t *)[M.occl contents])[slot] = 0;
    [M.enc setVisibilityResultMode:MTLVisibilityResultModeCounting offset:slot * 8];
    return (int)slot;
}

static void occl_slot_end(int slot)
{
    if (slot < 0)
        return;
    [M.enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    M.occl_serial = M.serial;
}

static int m_occlusion_begin(void)
{
    if (!M.occl)
        return -1;
    M.occl_on = 1;
    if (M.occl_next >= OCCL_SLOTS) {
        wait_serial(M.occl_serial);
        M.occl_next = 0;
        M.occl_gen++;
    }
    M.occl_first = M.occl_next;
    return 0;
}

static void m_occlusion_end(RhiOcclusion *out)
{
    M.occl_on = 0;
    if (!out)
        return;
    out->first = M.occl_first;
    out->count = M.occl_next >= M.occl_first ? M.occl_next - M.occl_first : 0;
    out->generation = M.occl_gen;
}

static int m_occlusion_result(const RhiOcclusion *t, uint64_t *samples)
{
    uint64_t sum = 0;
    uint32_t i;
    if (!t || !M.occl || t->generation != M.occl_gen)
        return -1;
    if (t->count)
        wait_serial(M.occl_serial);
    for (i = 0; i < t->count; i++)
        sum += ((const uint64_t *)[M.occl contents])[t->first + i];
    *samples = sum;
    return 0;
}

/* ---- the swap chain ---------------------------------------------------------------------------------- */

static int make_backbuffer(uint32_t w, uint32_t h)
{
    RhiImageDesc d;
    RhiImage *img;

    memset(&d, 0, sizeof d);
    d.type = RHI_IMAGE_2D;
    d.width = w ? w : 1;
    d.height = h ? h : 1;
    d.depth = 1;
    d.mip_levels = 1;
    d.format = RHI_FORMAT_B8G8R8A8_UNORM;
    d.samples = 1;
    d.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;
    img = m_image_create(&d, NULL);
    if (!img)
        return -1;
    if (!M.bb_view) {
        M.bb_view = m_view_create(img, RHI_VIEW_RENDER_TARGET, NULL);
        m_image_destroy(img);            /* the view holds it */
    } else {
        /* the renderer may hold the view: keep it, change what it shows */
        RhiImage *old = M.bb_view->img;
        if (M.rt_color == M.bb_view)
            end_encoder();
        M.bb_view->img = img;            /* the creation reference becomes the view's */
        m_image_destroy(old);
    }
    M.bb = M.bb_view->img;
    if (M.layer)
        M.layer.drawableSize = CGSizeMake(d.width, d.height);
    return 0;
}

static RhiView *m_swapchain_view(void) { return M.bb_view; }

static int m_swapchain_resize(uint32_t w, uint32_t h)
{
    if (M.bb && M.bb->d.width == w && M.bb->d.height == h)
        return 0;
    return make_backbuffer(w, h);
}

static void m_swapchain_set_vrr(int on) { (void)on; }

static void report_stats(void)
{
    if (++M.st.presents < 300)
        return;
    fprintf(stderr, "[RHI] metal: last %u frames: %u submits (%u waited on), %u discards "
            "(%u renamed, %u new buffers, %u waited for one), uploads %u direct %u staged, "
            "%u pipelines built\n", M.st.presents, M.st.submits, M.st.sync_waits, M.st.discards,
            M.st.renames, M.st.new_versions, M.st.version_waits, M.st.direct_uploads,
            M.st.staged_uploads, M.st.pipelines);
    fflush(stderr);
    memset(&M.st, 0, sizeof M.st);
}

static int32_t m_present(uint32_t interval)
{
    report_stats();
    @autoreleasepool {
        id<CAMetalDrawable> dr = nil;
        flush_clear(M.bb);
        end_encoder();
        if (M.layer) {
            int sync = interval != 0;
            int h;
            if (M.display_sync != sync) {
                M.layer.displaySyncEnabled = sync ? YES : NO;
                M.display_sync = sync;
            }
            h = rhi_wait_begin();
            dr = [M.layer nextDrawable];
            rhi_wait_end(h);
            if (dr) {
                id<MTLTexture> t = dr.texture;
                id<MTLBlitCommandEncoder> bl = [cmd() blitCommandEncoder];
                NSUInteger w = t.width < M.bb->d.width ? t.width : M.bb->d.width;
                NSUInteger ht = t.height < M.bb->d.height ? t.height : M.bb->d.height;
                [bl copyFromTexture:M.bb->tex sourceSlice:0 sourceLevel:0
                       sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, ht, 1)
                          toTexture:t destinationSlice:0 destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
                [bl endEncoding];
                M.bb->used = M.serial;
            }
        }
        if (dr) {
            submit(0, dr);          /* waits for the frame FRAMES back */
        } else {
            submit(0, nil);
        }
    }
    return 0;
}

static int m_swapchain_readback(void *dst, uint32_t pitch, uint32_t *w, uint32_t *h)
{
    uint32_t x, y;
    int rc;

    if (!M.bb)
        return -1;
    if (w) *w = M.bb->d.width;
    if (h) *h = M.bb->d.height;
    if (!dst)
        return 0;
    rc = m_image_readback(M.bb, 0, dst, pitch);
    if (rc == 0)            /* BGRA -> RGBA */
        for (y = 0; y < M.bb->d.height; y++) {
            uint8_t *p = (uint8_t *)dst + (size_t)y * pitch;
            for (x = 0; x < M.bb->d.width; x++, p += 4) {
                uint8_t t = p[0]; p[0] = p[2]; p[2] = t;
            }
        }
    return rc;
}

/* ---- the device -------------------------------------------------------------------------------------- */

static int make_dummies(void)
{
    static const uint8_t zero[4 * 6 * 4] = { 0 };
    RhiSubresourceData sd[6];
    RhiImageDesc d;
    int k;

    for (k = 0; k < 6; k++) {
        sd[k].data = zero;
        sd[k].row_pitch = 4;
        sd[k].slice_pitch = 4;
    }
    for (k = 0; k < 5; k++) {
        RhiViewDesc vd;
        memset(&d, 0, sizeof d);
        d.type = k == TEX_3D ? RHI_IMAGE_3D : RHI_IMAGE_2D;
        d.width = d.height = 1;
        d.depth = k == TEX_2D_ARRAY ? 2 : k == TEX_CUBE ? 6 : 1;
        d.mip_levels = 1;
        d.format = RHI_FORMAT_R8G8B8A8_UNORM;
        d.samples = k == TEX_2D_MS ? 4 : 1;
        d.bind = RHI_BIND_SAMPLED | (k == TEX_2D_MS ? RHI_BIND_RENDER_TARGET : 0);
        d.cube = k == TEX_CUBE;
        M.dummy_img[k] = m_image_create(&d, k == TEX_2D_MS ? NULL : sd);
        if (!M.dummy_img[k])
            continue;
        memset(&vd, 0, sizeof vd);
        vd.dim = k == TEX_2D ? RHI_VIEW_DIM_2D : k == TEX_2D_ARRAY ? RHI_VIEW_DIM_2D_ARRAY :
                 k == TEX_CUBE ? RHI_VIEW_DIM_CUBE : k == TEX_3D ? RHI_VIEW_DIM_3D : RHI_VIEW_DIM_2D_MS;
        vd.mip_count = 1;
        vd.layer_count = d.depth;
        M.dummy[k] = m_view_create(M.dummy_img[k], RHI_VIEW_SAMPLED, &vd);
    }
    M.zero = [M.dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    memset([M.zero contents], 0, 65536);
    {
        MTLSamplerDescriptor *s = [[MTLSamplerDescriptor alloc] init];   /* D3D11's default */
        s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
        s.mipFilter = MTLSamplerMipFilterLinear;
        s.sAddressMode = s.tAddressMode = s.rAddressMode = MTLSamplerAddressModeClampToEdge;
        M.def_sampler = [M.dev newSamplerStateWithDescriptor:s];
        [s release];
    }
    {
        MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = NO;
        M.no_depth = [M.dev newDepthStencilStateWithDescriptor:dd];
        [dd release];
    }
    M.occl = [M.dev newBufferWithLength:OCCL_SLOTS * 8 options:MTLResourceStorageModeShared];
    return M.zero && M.def_sampler && M.no_depth ? 0 : -1;
}

static int m_device_create(const RhiDeviceDesc *dd)
{
    memset(&M, 0, sizeof M);
    M.serial = 1;
    atomic_store(&M.done, 0);
    M.display_sync = -1;
    M.trace = env_on("RECOMP_METAL_TRACE");
    @autoreleasepool {
        if (dd->window) {
            M.layer = [(CAMetalLayer *)dd->window retain];
            M.dev = M.layer.device ? [M.layer.device retain] : MTLCreateSystemDefaultDevice();
        } else {
            M.dev = MTLCreateSystemDefaultDevice();
        }
        if (!M.dev) {
            fprintf(stderr, "[RHI] metal: no Metal device\n");
            return -1;
        }
        M.unified = M.dev.hasUnifiedMemory ? 1 : 0;
        M.q = [M.dev newCommandQueue];
        if (M.layer) {
            M.layer.device = M.dev;
            M.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            M.layer.framebufferOnly = NO;        /* the back buffer is copied in */
            M.layer.maximumDrawableCount = 3;
            M.layer.allowsNextDrawableTimeout = YES;
        }
        {
            /* the layer's own pixels, as a Vulkan surface reports them: a
             * Retina window asked for at 640x480 is 1280x960 */
            uint32_t w = dd->width, h = dd->height;
            if (M.layer) {
                CGSize px = M.layer.bounds.size;
                CGFloat sc = M.layer.contentsScale > 0 ? M.layer.contentsScale : 1;
                if (px.width * sc >= 1 && px.height * sc >= 1) {
                    w = (uint32_t)(px.width * sc + 0.5);
                    h = (uint32_t)(px.height * sc + 0.5);
                }
            }
            if (make_dummies() != 0 || make_backbuffer(w, h) != 0) {
                fprintf(stderr, "[RHI] metal: device set-up failed\n");
                return -1;
            }
            fprintf(stderr, "[RHI] metal: %s, %s memory, swap chain %ux%u (asked %ux%u)%s\n",
                    M.dev.name.UTF8String, M.unified ? "unified" : "discrete", w, h,
                    dd->width, dd->height, M.layer ? "" : ", headless");
        }
        fflush(stderr);
    }
    return 0;
}

static void m_device_destroy(void)
{
    uint32_t i;
    Page *p, *n;

    if (!M.dev)
        return;
    @autoreleasepool {
        submit(1, nil);
        for (i = 0; i < 1024; i++) {
            Pipe *q = M.pipes[i], *nx;
            for (; q; q = nx) {
                nx = q->next;
                [q->state release];
                free(q);
            }
        }
        for (i = 0; i < 5; i++) {
            m_view_destroy(M.dummy[i]);
            m_image_destroy(M.dummy_img[i]);
        }
        m_view_destroy(M.bb_view);
        for (p = M.pages; p; p = n) {
            n = p->next;
            [p->buf release];
            free(p);
        }
        [M.zero release];
        [M.occl release];
        [M.def_sampler release];
        [M.no_depth release];
        [M.q release];
        [M.layer release];
        [M.dev release];
        {
            int k;
            for (k = 0; k < FRAMES; k++) {
                [M.frame_cb[k] waitUntilCompleted];
                [M.frame_cb[k] release];
            }
        }
    }
    memset(&M, 0, sizeof M);
}

static int m_device_ready(void) { return M.dev != nil; }

const RhiBackend rhi_metal_backend = {
    "metal",
    m_device_create,
    m_device_destroy,
    m_device_ready,
    m_swapchain_view,
    m_swapchain_resize,
    m_present,
    m_swapchain_readback,
    m_swapchain_set_vrr,
    m_buffer_create,
    m_buffer_destroy,
    m_buffer_map,
    m_buffer_unmap,
    m_buffer_update,
    m_image_create,
    m_image_retain,
    m_image_destroy,
    m_image_get_desc,
    m_image_update,
    m_image_readback,
    m_view_create,
    m_view_destroy,
    m_view_image,
    m_sample_count_supported,
    m_shader_create,
    m_shader_destroy,
    m_vertex_layout_create,
    m_vertex_layout_destroy,
    m_blend_state_create,
    m_depth_state_create,
    m_raster_state_create,
    m_sampler_create,
    m_blend_state_destroy,
    m_depth_state_destroy,
    m_raster_state_destroy,
    m_sampler_destroy,
    m_set_render_target,
    m_output_save,
    m_output_restore,
    m_output_color_is,
    m_set_viewports,
    m_set_scissor,
    m_set_topology,
    m_set_vertex_layout,
    m_set_vertex_buffer,
    m_set_index_buffer,
    m_set_shader,
    m_set_uniform_buffers,
    m_set_textures,
    m_set_samplers,
    m_set_blend_state,
    m_set_depth_state,
    m_set_raster_state,
    m_draw,
    m_draw_indexed,
    m_clear_color,
    m_clear_depth,
    m_image_copy,
    m_occlusion_begin,
    m_occlusion_end,
    m_occlusion_result,
};
