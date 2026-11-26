/*
 * hle_menu.cpp -- the in-game menu (hle_menu.h says what it is for).
 *
 * Three parts:
 *   input     events from the window's thread, queued under a spin lock and
 *             handed to Dear ImGui by the presenting thread, the only one
 *             that touches the ImGui context
 *   drawing   ImGui's draw lists through the RHI, over the swap chain, after
 *             everything else (xbox_D3D8SetUiHook). Like the frame counter it
 *             binds no sampler: the pixel shader reads the font atlas by
 *             Load, at whole texels, and the font is rasterised at the size
 *             it is drawn. It sets no scissor either -- the title's own
 *             scissor is the device's to keep -- and clips in the pixel
 *             shader instead, one draw per ImGui command.
 *   the menu  what it shows, and what each choice does to the runtime and
 *             the settings file
 *
 * Holding the title: opening sets HOST_SUSPEND_MENU, and the title stops at
 * its next Swap (the flip gate, src/kernel/xbox_memory_layout.c). While it is
 * held, the flip gate presents the held frame again whenever the host asks
 * (host_request_redraw), which this asks for on every input, so the menu
 * keeps answering over a still picture. Not in an online co-op session
 * (recomp_lockstep_active): the other machine's game cannot wait, so there
 * the game goes on and the menu only takes the keyboard and mouse.
 */
#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "hle_menu.h"
#include "hle_settings.h"
#include "audio_output.h"
#include "input_bindings.h"
#include "recomp_config.h"
#include "recomp_lockstep.h"
#include "rhi.h"

/* d3d8_xbox.h, platform/host_main.h and kernel_bridge.c: declared here rather
 * than included, since the first is the Xbox's own types and C only. */
void xbox_D3D8SetUiHook(void (*draw)(void *out_view, unsigned width, unsigned height));
void xbox_D3D8SetPostSoften(int mode);
void xbox_D3D8SetPicture(int brightness, int contrast, int saturation, int gamma);
void host_set_suspended(unsigned reason, int on);
void host_request_redraw(void);
int  host_exe_path(char *buf, size_t bytes);
int  host_launch_other(const char *exe);
void xbox_HostExit(const char *why);
}

#define MENU_HOST_SUSPEND_MENU 2u    /* HOST_SUSPEND_MENU, platform/host_main.h */

/* ------------------------------------------------------------------ input */

namespace {

enum { EV_POS, EV_BUTTON, EV_WHEEL, EV_KEY, EV_CHAR };

struct Event {
    int      type;
    int      a, b;
    float    x, y;
    unsigned c;
};

constexpr int kQueueMax = 256;
Event            g_queue[kQueueMax];
int              g_queued;
std::atomic_flag g_queue_lock = ATOMIC_FLAG_INIT;
std::atomic<int> g_open{0};
std::atomic<int> g_held{0};          /* this opening holds the title */
std::atomic<int> g_reload{0};        /* read the settings file again: just opened */
std::atomic<int> g_leaving{0};       /* just closed: save what is not saved yet */

void queue_push(const Event &e)
{
    if (!g_open.load())
        return;
    while (g_queue_lock.test_and_set(std::memory_order_acquire)) {}
    if (g_queued < kQueueMax)
        g_queue[g_queued++] = e;
    g_queue_lock.clear(std::memory_order_release);
    host_request_redraw();
}

ImGuiKey imgui_key(int key)
{
    switch (key) {
    case RECOMP_MENU_KEY_TAB:       return ImGuiKey_Tab;
    case RECOMP_MENU_KEY_LEFT:      return ImGuiKey_LeftArrow;
    case RECOMP_MENU_KEY_RIGHT:     return ImGuiKey_RightArrow;
    case RECOMP_MENU_KEY_UP:        return ImGuiKey_UpArrow;
    case RECOMP_MENU_KEY_DOWN:      return ImGuiKey_DownArrow;
    case RECOMP_MENU_KEY_PAGE_UP:   return ImGuiKey_PageUp;
    case RECOMP_MENU_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
    case RECOMP_MENU_KEY_HOME:      return ImGuiKey_Home;
    case RECOMP_MENU_KEY_END:       return ImGuiKey_End;
    case RECOMP_MENU_KEY_DELETE:    return ImGuiKey_Delete;
    case RECOMP_MENU_KEY_BACKSPACE: return ImGuiKey_Backspace;
    case RECOMP_MENU_KEY_SPACE:     return ImGuiKey_Space;
    case RECOMP_MENU_KEY_ENTER:     return ImGuiKey_Enter;
    case RECOMP_MENU_KEY_ESCAPE:    return ImGuiKey_Escape;
    case RECOMP_MENU_KEY_SHIFT:     return ImGuiMod_Shift;
    case RECOMP_MENU_KEY_CTRL:      return ImGuiMod_Ctrl;
    case RECOMP_MENU_KEY_ALT:       return ImGuiMod_Alt;
    default:                        return ImGuiKey_None;
    }
}

/* Hands what arrived to ImGui. The presenting thread, before NewFrame.
 * Returns how many events it handed over. */
int queue_drain(ImGuiIO &io)
{
    Event local[kQueueMax];
    int n;

    while (g_queue_lock.test_and_set(std::memory_order_acquire)) {}
    n = g_queued;
    memcpy(local, g_queue, sizeof(Event) * (size_t)n);
    g_queued = 0;
    g_queue_lock.clear(std::memory_order_release);
    for (int i = 0; i < n; i++) {
        const Event &e = local[i];

        switch (e.type) {
        case EV_POS:    io.AddMousePosEvent(e.x, e.y); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL:  io.AddMouseWheelEvent(0.0f, e.y); break;
        case EV_KEY: {
            ImGuiKey k = imgui_key(e.a);

            if (k != ImGuiKey_None)
                io.AddKeyEvent(k, e.b != 0);
            break;
        }
        case EV_CHAR:   io.AddInputCharacter(e.c); break;
        }
    }
    return n;
}

} // namespace

extern "C" void recomp_menu_mouse_pos(float x, float y)
{
    Event e = {EV_POS, 0, 0, x, y, 0};
    queue_push(e);
}

extern "C" void recomp_menu_mouse_button(int button, int down)
{
    Event e = {EV_BUTTON, button, down, 0, 0, 0};
    if (button >= 0 && button < 3)
        queue_push(e);
}

extern "C" void recomp_menu_mouse_wheel(float dy)
{
    Event e = {EV_WHEEL, 0, 0, 0, dy, 0};
    queue_push(e);
}

extern "C" void recomp_menu_key(int key, int down)
{
    Event e = {EV_KEY, key, down, 0, 0, 0};
    queue_push(e);
}

extern "C" void recomp_menu_char(unsigned codepoint)
{
    Event e = {EV_CHAR, 0, 0, 0, 0, codepoint};
    if (codepoint >= 32)
        queue_push(e);
}

/* ------------------------------------------------------- opening, closing */

extern "C" int recomp_menu_is_open(void)
{
    return g_open.load();
}

extern "C" void recomp_menu_set_open(int open)
{
    int want = open ? 1 : 0;

    if (g_open.exchange(want) == want)
        return;
    recomp_bindings_block(want);
    if (want) {
        /* Online, the other machine's game cannot wait for this one. */
        int hold = !recomp_lockstep_active();

        g_held.store(hold);
        g_reload.store(1);
        if (hold) {
            host_set_suspended(MENU_HOST_SUSPEND_MENU, 1);
            recomp_audio_output_pause(1);
        }
        fprintf(stderr, "[MENU] open%s\n", hold ? ": the title is held at its next frame"
                                                : " (online: the game goes on)");
    } else {
        g_leaving.store(1);
        if (g_held.exchange(0)) {
            recomp_audio_output_pause(0);
            host_set_suspended(MENU_HOST_SUSPEND_MENU, 0);
        }
        fprintf(stderr, "[MENU] closed\n");
    }
    fflush(stderr);
    hle_window_menu_pointer(want);
    host_request_redraw();
}

extern "C" void recomp_menu_toggle(void)
{
    recomp_menu_set_open(!recomp_menu_is_open());
}

/* ---------------------------------------------------------------- drawing */

namespace {

/* b7 and t8, as the frame counter uses: past the title's own slots. */
struct Constants {
    float scale[2], offset[2];     /* pixels to clip space */
    float clip[4];                 /* x0, y0, x1, y1 in pixels: the command's clip rect */
    float tex[4];                  /* the atlas's width and height */
};

const char kShader[] =
    "cbuffer Menu : register(b7) {\n"
    "    float4 xform;   // scale.xy, offset.zw\n"
    "    float4 clip;    // x0, y0, x1, y1 in pixels\n"
    "    float4 tex;     // atlas width, height\n"
    "};\n"
    "struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };\n"
    "struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(VSIn i) {\n"
    "    VSOut o;\n"
    "    o.pos = float4(i.pos * xform.xy + xform.zw, 0, 1);\n"
    "    o.col = i.col;\n"
    "    o.uv = i.uv;\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> atlas : register(t8);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    if (i.pos.x < clip.x || i.pos.y < clip.y || i.pos.x >= clip.z || i.pos.y >= clip.w)\n"
    "        discard;\n"
    "    int2 p = int2(min(i.uv * tex.xy, tex.xy - 1));\n"
    "    return i.col * atlas.Load(int3(p, 0));\n"
    "}\n";

struct Gpu {
    bool            tried, failed;
    RhiShader      *vs, *ps;
    RhiVertexLayout *layout;
    RhiBuffer      *cb, *vb, *ib;
    uint32_t        vb_size, ib_size;       /* in elements */
    RhiImage       *atlas;
    RhiView        *atlas_view;
    float           atlas_w, atlas_h;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
} g;

ImGuiContext *g_ctx;
float         g_font_px;                    /* the size the atlas holds */
float         g_font_px_drawn;              /* the size the menu was last laid out at */
ImGuiStyle    g_base_style;

void gpu_fail(const char *what)
{
    g.failed = true;
    fprintf(stderr, "[MENU] %s failed: the in-game menu cannot be drawn this run\n", what);
    fflush(stderr);
}

RhiShader *compile(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    char err[2048];
    RhiShader *s;

    memset(&src, 0, sizeof src);
    src.hlsl = kShader;
    src.len = sizeof kShader - 1;
    src.name = "menu";
    src.entry = entry;
    src.target = target;
    err[0] = '\0';
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s && err[0])
        fprintf(stderr, "[MENU] %s\n", err);
    return s;
}

bool gpu_create()
{
    if (g.failed)
        return false;
    if (g.tried)
        return true;
    g.tried = true;
    if (!rhi_device_ready()) {
        gpu_fail("the renderer");
        return false;
    }
    if (!(g.vs = compile(RHI_STAGE_VERTEX, "vs_main", "vs_4_0")) ||
        !(g.ps = compile(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) {
        gpu_fail("compiling its shaders");
        return false;
    }
    RhiVertexElement el[3] = {
        {"POSITION", 0, RHI_FORMAT_R32G32_FLOAT,   0, 0},
        {"TEXCOORD", 0, RHI_FORMAT_R32G32_FLOAT,   0, 8},
        {"COLOR",    0, RHI_FORMAT_R8G8B8A8_UNORM, 0, 16},
    };
    if (!(g.layout = rhi_vertex_layout_create(el, 3, g.vs))) {
        gpu_fail("its vertex layout");
        return false;
    }
    RhiBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(Constants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, nullptr))) {
        gpu_fail("its constant buffer");
        return false;
    }
    RhiBlendDesc bl;
    memset(&bl, 0, sizeof bl);
    bl.enable = 1;
    bl.src = RHI_BLEND_SRC_ALPHA;
    bl.dst = RHI_BLEND_INV_SRC_ALPHA;
    bl.op = RHI_BLEND_OP_ADD;
    bl.src_alpha = RHI_BLEND_ONE;
    bl.dst_alpha = RHI_BLEND_INV_SRC_ALPHA;
    bl.op_alpha = RHI_BLEND_OP_ADD;
    bl.write_mask = RHI_WRITE_ALL;
    RhiDepthDesc ds;
    memset(&ds, 0, sizeof ds);
    RhiRasterDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.blend = rhi_blend_state_create(&bl)) || !(g.depth = rhi_depth_state_create(&ds)) ||
        !(g.raster = rhi_raster_state_create(&rd))) {
        gpu_fail("its render states");
        return false;
    }
    return true;
}

/* Room for at least `need` vertices or indices; grown by half again. */
bool ensure_buffer(RhiBuffer **b, uint32_t *have, uint32_t need, uint32_t stride, uint32_t bind)
{
    if (*b && *have >= need)
        return true;
    rhi_buffer_destroy(*b);
    *b = nullptr;
    uint32_t n = need + need / 2 + 256;
    RhiBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = n * stride;
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = bind;
    bd.cpu_access = RHI_CPU_WRITE;
    *b = rhi_buffer_create(&bd, nullptr);
    *have = *b ? n : 0;
    return *b != nullptr;
}

const char *font_file()
{
    static const char *const candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
#else
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
#endif
    };
#if defined(_WIN32)
    static char windir_font[512];
    const char *windir = getenv("WINDIR");
    if (windir && *windir) {
        snprintf(windir_font, sizeof windir_font, "%s\\Fonts\\segoeui.ttf", windir);
        if (FILE *f = fopen(windir_font, "rb")) {
            fclose(f);
            return windir_font;
        }
    }
#endif
    for (const char *p : candidates)
        if (FILE *f = fopen(p, "rb")) {
            fclose(f);
            return p;
        }
    return nullptr;
}

void apply_style(float scale)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = g_base_style;
    st.ScaleAllSizes(scale);
}

void make_base_style()
{
    ImGui::StyleColorsDark(&g_base_style);
    ImGuiStyle &st = g_base_style;
    st.WindowRounding = 10.0f;
    st.FrameRounding = 6.0f;
    st.GrabRounding = 6.0f;
    st.TabRounding = 6.0f;
    st.PopupRounding = 8.0f;
    st.WindowPadding = ImVec2(22, 18);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(12, 10);
    st.WindowBorderSize = 0.0f;
    st.AntiAliasedLinesUseTex = false;          /* the atlas is read unfiltered */
    ImVec4 *c = st.Colors;
    const ImVec4 accent(0.96f, 0.62f, 0.16f, 1.00f), accent_dim(0.70f, 0.44f, 0.10f, 1.00f);
    c[ImGuiCol_WindowBg]         = ImVec4(0.07f, 0.07f, 0.08f, 0.94f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.09f, 0.09f, 0.10f, 0.98f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.24f, 0.22f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.30f, 0.26f, 0.20f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = accent_dim;
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = ImVec4(0.22f, 0.20f, 0.18f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = accent_dim;
    c[ImGuiCol_HeaderActive]     = accent;
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accent_dim;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Tab]              = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_TabHovered]       = accent_dim;
    c[ImGuiCol_TabSelected]      = ImVec4(0.30f, 0.22f, 0.12f, 1.00f);
    c[ImGuiCol_NavCursor]        = accent;
    c[ImGuiCol_Separator]        = ImVec4(0.25f, 0.25f, 0.27f, 1.00f);
}

/* The atlas at a size for this window: rebuilt when the window's height
 * calls for a different one. */
bool ensure_font(float want_px)
{
    if (g.atlas && g_font_px == want_px)
        return true;
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
    const char *path = font_file();
    ImFont *font = nullptr;
    if (path)
        font = io.Fonts->AddFontFromFileTTF(path, want_px);
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = want_px;
        io.Fonts->AddFontDefault(&cfg);
    }
    unsigned char *pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    rhi_view_destroy(g.atlas_view);
    rhi_image_destroy(g.atlas);
    g.atlas_view = nullptr;
    g.atlas = nullptr;
    RhiImageDesc td;
    memset(&td, 0, sizeof td);
    td.type = RHI_IMAGE_2D;
    td.width = (uint32_t)w;
    td.height = (uint32_t)h;
    td.depth = 1;
    td.mip_levels = 1;
    td.format = RHI_FORMAT_R8G8B8A8_UNORM;
    td.samples = 1;
    td.usage = RHI_USAGE_IMMUTABLE;
    td.bind = RHI_BIND_SAMPLED;
    RhiSubresourceData init = {pixels, (uint32_t)w * 4u, (uint32_t)(w * h) * 4u};
    if (!(g.atlas = rhi_image_create(&td, &init)) ||
        !(g.atlas_view = rhi_view_create(g.atlas, RHI_VIEW_SAMPLED, nullptr))) {
        gpu_fail("its font");
        return false;
    }
    g.atlas_w = (float)w;
    g.atlas_h = (float)h;
    g_font_px = want_px;
    apply_style(want_px / 18.0f);
    fprintf(stderr, "[MENU] font %s at %.0f px\n", path ? path : "(built in)", want_px);
    return true;
}

void render(ImDrawData *dd, RhiView *out, unsigned width, unsigned height)
{
    if (!dd || dd->TotalVtxCount <= 0)
        return;
    if (!ensure_buffer(&g.vb, &g.vb_size, (uint32_t)dd->TotalVtxCount, sizeof(ImDrawVert),
                       RHI_BIND_VERTEX) ||
        !ensure_buffer(&g.ib, &g.ib_size, (uint32_t)dd->TotalIdxCount, sizeof(ImDrawIdx),
                       RHI_BIND_INDEX))
        return;
    ImDrawVert *vtx = (ImDrawVert *)rhi_buffer_map(g.vb, RHI_MAP_WRITE_DISCARD);
    ImDrawIdx *idx = (ImDrawIdx *)rhi_buffer_map(g.ib, RHI_MAP_WRITE_DISCARD);
    if (!vtx || !idx) {
        if (vtx) rhi_buffer_unmap(g.vb);
        if (idx) rhi_buffer_unmap(g.ib);
        return;
    }
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *l = dd->CmdLists[n];
        memcpy(vtx, l->VtxBuffer.Data, (size_t)l->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idx, l->IdxBuffer.Data, (size_t)l->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtx += l->VtxBuffer.Size;
        idx += l->IdxBuffer.Size;
    }
    rhi_buffer_unmap(g.vb);
    rhi_buffer_unmap(g.ib);

    RhiOutputState saved;
    rhi_output_save(&saved);
    RhiViewport vp = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
    float blend_factor[4] = {1, 1, 1, 1};
    rhi_set_render_target(out, nullptr);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(g.layout);
    rhi_set_vertex_buffer(0, g.vb, sizeof(ImDrawVert), 0);
    rhi_set_index_buffer(g.ib, sizeof(ImDrawIdx) * 8, 0);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, 7, 1, &g.cb);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(8, 1, &g.atlas_view);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);

    Constants c;
    c.scale[0] = 2.0f / (float)width;
    c.scale[1] = -2.0f / (float)height;
    c.offset[0] = -1.0f;
    c.offset[1] = 1.0f;
    c.tex[0] = g.atlas_w;
    c.tex[1] = g.atlas_h;
    c.tex[2] = c.tex[3] = 0.0f;
    int vbase = 0, ibase = 0;
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *l = dd->CmdLists[n];
        for (int i = 0; i < l->CmdBuffer.Size; i++) {
            const ImDrawCmd *cmd = &l->CmdBuffer[i];
            if (cmd->UserCallback || !cmd->ElemCount)
                continue;
            c.clip[0] = cmd->ClipRect.x;
            c.clip[1] = cmd->ClipRect.y;
            c.clip[2] = cmd->ClipRect.z;
            c.clip[3] = cmd->ClipRect.w;
            if (c.clip[2] <= c.clip[0] || c.clip[3] <= c.clip[1])
                continue;
            void *m = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD);
            if (!m)
                continue;
            memcpy(m, &c, sizeof c);
            rhi_buffer_unmap(g.cb);
            rhi_draw_indexed(cmd->ElemCount, (uint32_t)ibase + cmd->IdxOffset,
                             vbase + (int32_t)cmd->VtxOffset);
        }
        vbase += l->VtxBuffer.Size;
        ibase += l->IdxBuffer.Size;
    }
    rhi_output_restore(&saved);
}

/* ------------------------------------------------------------- the menu */

const char *const kCaps[] = {"adaptive", "60", "30", "off"};
const char *const kCapNames[] = {"Adaptive (60, catches up)", "60", "30", "Off (the game runs fast)"};
const char *const kInterpNames[] = {"Off (60)", "2x (120)", "3x (180)", "4x (240)"};
const char *const kScaleNames[] = {"1x (640x480)", "2x (1280x960)", "3x (1920x1440)",
                                   "4x (2560x1920)", "5x", "6x", "7x", "8x"};
const int kAniso[] = {1, 2, 4, 8, 16};
const char *const kAnisoNames[] = {"Off", "2x", "4x", "8x", "16x"};
/* post_soften: in the device's order (xbox_D3D8SetPostSoften) */
const char *const kSoften[] = {"console", "sharp", "off"};
const char *const kSoftenNames[] = {"As the console", "Sharp", "Off"};

struct Menu {
    bool           loaded;
    char           path[1024];
    bool           have_path;
    RecompSettings st;           /* what the file says now */
    RecompSettings started;      /* what this run started with */
    bool           confirm_quit;
    bool           confirm_restart;
    bool           unsaved;      /* changed here and not in the file yet (Apply) */
    bool           picture_tab;  /* the Picture tab is showing: the game undimmed */
    char           test_tab[16]; /* RECOMP_MENU_TEST: the tab to open on, once */
    bool           test_on;      /* RECOMP_MENU_TEST: keep presenting while open */
} m;

void settings_load()
{
    recomp_settings_defaults(&m.st);
    m.have_path = recomp_settings_path(recomp_config_title_id(), m.path, sizeof m.path) != 0;
    if (m.have_path)
        recomp_settings_read(m.path, &m.st);
    if (!m.loaded)
        m.started = m.st;
    m.loaded = true;
}

void settings_save()
{
    if (m.have_path && !recomp_settings_write(m.path, &m.st))
        fprintf(stderr, "[MENU] could not save %s\n", m.path);
    else
        m.unsaved = false;
}

/* Whatever is not saved yet, saved: on Apply, on leaving the menu, on quitting. */
void settings_save_unsaved()
{
    if (m.loaded && m.unsaved) {
        settings_save();
        fprintf(stderr, "[MENU] settings saved\n");
    }
}

/* The renderer a settings value means: "auto" (or none) is the title's own
 * if it names one, else Direct3D 11 -- as rhi_select_backend reads it. */
const char *renderer_meant(const char *r)
{
    if (!r[0] || !strcmp(r, "auto")) {
        const char *t = recomp_title_setting_default("renderer");
        return t && *t && strcmp(t, "auto") ? t : "d3d11";
    }
    return r;
}

bool restart_pending()
{
    return strcmp(renderer_meant(m.st.renderer), renderer_meant(m.started.renderer)) != 0 ||
           m.st.resolution_scale != m.started.resolution_scale ||
           m.st.widescreen != m.started.widescreen ||
           m.st.anisotropy != m.started.anisotropy ||
           m.st.frame_interp != m.started.frame_interp ||
           m.st.vrr != m.started.vrr;
}

void next_start_note()
{
    ImGui::SameLine();
    ImGui::TextDisabled("(next start)");
}

int cap_index(const char *name)
{
    for (int i = 0; i < 4; i++)
        if (!strcmp(name, kCaps[i]))
            return i;
    return 0;
}

/* The field of view (hle_settings.h): the setting, read once and then the
 * menu's, and whether the title applies it. */
std::atomic<int> g_fov{-1};
int g_fov_title;

} // namespace

extern "C" void hle_fov_title_default(int degrees)
{
    g_fov_title = degrees > 0 ? degrees : 0;
}

extern "C" int hle_fov_title_has(void)
{
    return g_fov_title > 0;
}

extern "C" int hle_fov(void)
{
    int v = g_fov.load();

    if (v < 0) {
        v = recomp_config_int("RECOMP_FOV", "fov", 0);
        v = v <= 0 ? 0 : v < RECOMP_FOV_MIN ? RECOMP_FOV_MIN : v > RECOMP_FOV_MAX ? RECOMP_FOV_MAX : v;
        g_fov.store(v);
    }
    return v;
}

extern "C" void hle_set_fov(int degrees)
{
    g_fov.store(degrees <= 0 ? 0 : degrees);
}

/* The crosshair setting (recomp_config.h crosshair): read once, then the
 * menu's. A title that has an aiming crosshair asks each frame. */
namespace { std::atomic<int> g_crosshair{-1}; }

extern "C" int hle_crosshair_always(void)
{
    int v = g_crosshair.load();

    if (v < 0) {
        v = recomp_config_int("RECOMP_CROSSHAIR", "crosshair", 1) ? 1 : 0;
        g_crosshair.store(v);
    }
    return v;
}

namespace {

void tab_display()
{
    bool fs = hle_window_fullscreen() != 0;
    if (ImGui::Checkbox("Fullscreen", &fs)) {
        hle_window_set_fullscreen(fs);
        m.st.fullscreen = fs;
        settings_save();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Alt+Enter");

    int scale = m.st.resolution_scale < 1 ? 0 : m.st.resolution_scale > 8 ? 7 : m.st.resolution_scale - 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Resolution", &scale, kScaleNames, 8)) {
        m.st.resolution_scale = scale + 1;
        settings_save();
    }
    if (m.st.resolution_scale != m.started.resolution_scale)
        next_start_note();

    bool wide = m.st.widescreen != 0;
    if (ImGui::Checkbox("Widescreen (16:9)", &wide)) {
        recomp_settings_set_widescreen(&m.st, wide);
        settings_save();
    }
    if (m.st.widescreen != m.started.widescreen)
        next_start_note();

    int an = 0;
    for (int i = 0; i < 5; i++)
        if (m.st.anisotropy >= kAniso[i])
            an = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Texture filtering", &an, kAnisoNames, 5)) {
        m.st.anisotropy = kAniso[an];
        settings_save();
    }
    if (m.st.anisotropy != m.started.anisotropy)
        next_start_note();

    /* The soften pass: at once. */
    int so = 1;
    for (int i = 0; i < 3; i++)
        if (strcmp(m.st.post_soften, kSoften[i]) == 0)
            so = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Post-processing", &so, kSoftenNames, 3)) {
        snprintf(m.st.post_soften, sizeof m.st.post_soften, "%s", kSoften[so]);
        xbox_D3D8SetPostSoften(so);
        settings_save();
    }

    if (hle_fov_title_has()) {
        /* Vertical degrees, with how wide that is on this screen's shape. */
        int fov = hle_fov() > 0 ? hle_fov() : g_fov_title;
        float aspect = m.st.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        float wide = 2.0f * atanf(tanf((float)fov * 0.00872665f) * aspect) * 57.2957795f;
        char label[64];
        bool online = !g_held.load();

        snprintf(label, sizeof label, "%d\xc2\xb0 (%.0f\xc2\xb0 across)", fov, wide);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (online)
            ImGui::BeginDisabled();
        if (ImGui::SliderInt("Field of view", &fov, RECOMP_FOV_MIN, RECOMP_FOV_MAX, label)) {
            int v = fov == g_fov_title ? 0 : fov;
            hle_set_fov(v);
            m.st.fov = v;
            settings_save();
        }
        if (online)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (online)
            ImGui::TextDisabled("(both players' must match)");
        else if (ImGui::SmallButton("Default")) {
            hle_set_fov(0);
            m.st.fov = 0;
            settings_save();
        }
    }
    /* Advanced: what a player seldom needs, kept out of the way. */
#if defined(_WIN32)
    ImGui::Spacing();
    if (!strcmp(m.test_tab, "Advanced"))
        ImGui::SetNextItemOpen(true);
    if (ImGui::CollapsingHeader("Advanced")) {
        /* The renderer: Vulkan or Direct3D 11, from the next start. Which one
         * is the default is the title's (Timeslip: Vulkan); the other is kept
         * here, under Advanced, for a machine the default does not suit. */
        {
            const bool vk_default = !strcmp(renderer_meant(""), "vulkan");
            const char *names[2] = {vk_default ? "Vulkan (default)" : "Vulkan",
                                    vk_default ? "Direct3D 11" : "Direct3D 11 (default)"};
            int r = !strcmp(renderer_meant(m.st.renderer), "vulkan") ? 0 : 1;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
            if (ImGui::Combo("Renderer", &r, names, 2)) {
                snprintf(m.st.renderer, sizeof m.st.renderer, "%s", r == 0 ? "vulkan" : "d3d11");
                settings_save();
            }
            if (strcmp(renderer_meant(m.st.renderer), renderer_meant(m.started.renderer)) != 0)
                next_start_note();
            else {
                ImGui::SameLine();
                ImGui::TextDisabled("(drawing with %s)",
                                    !strcmp(rhi_backend_name(), "vulkan") ? "Vulkan" :
                                    !strcmp(rhi_backend_name(), "d3d11") ? "Direct3D 11" : rhi_backend_name());
            }
        }
    }
#endif
}

void tab_frame_rate()
{
    int cap = cap_index(hle_frame_cap());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Frame cap", &cap, kCapNames, 4)) {
        hle_set_frame_cap(kCaps[cap]);
        snprintf(m.st.frame_cap, sizeof m.st.frame_cap, "%s", cap == 3 ? "0" : kCaps[cap]);
        settings_save();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F10");
    ImGui::TextWrapped("The game's own logic steps once a frame, so a cap above 60 makes it run "
                       "fast. For smoother motion, use frame interpolation.");

    int fi = m.st.frame_interp < 1 ? 0 : m.st.frame_interp > 4 ? 3 : m.st.frame_interp - 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::Combo("Frame interpolation", &fi, kInterpNames, 4)) {
        m.st.frame_interp = fi + 1;
        settings_save();
    }
    if (m.st.frame_interp != m.started.frame_interp)
        next_start_note();

    bool vrr = m.st.vrr != 0;
    if (ImGui::Checkbox("Variable refresh rate (G-SYNC, FreeSync)", &vrr)) {
        m.st.vrr = vrr;
        settings_save();
    }
    if (m.st.vrr != m.started.vrr)
        next_start_note();

    bool fps = hle_fps_overlay() != 0;
    if (ImGui::Checkbox("Show the frame rate", &fps)) {
        hle_set_fps_overlay(fps);
        m.st.fps_overlay = fps;
        settings_save();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F9");
}

void tab_mouse()
{
    bool look = m.st.mouse_look != 0;
    int sens = m.st.mouse_sensitivity;
    bool inv = m.st.mouse_invert != 0;
    bool changed = false;

    changed |= ImGui::Checkbox("Mouse look", &look);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    changed |= ImGui::SliderInt("Sensitivity", &sens, RECOMP_MOUSE_SENSITIVITY_MIN,
                                RECOMP_MOUSE_SENSITIVITY_MAX);
    changed |= ImGui::Checkbox("Invert vertical look", &inv);
    bool cross = hle_crosshair_always() != 0;
    bool locked = !g_held.load();            /* online: both players' must match */
    if (locked)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Crosshair always on", &cross)) {
        g_crosshair.store(cross ? 1 : 0);
        m.st.crosshair = cross;
        settings_save();
    }
    if (locked)
        ImGui::EndDisabled();
    if (changed) {
        m.st.mouse_look = look;
        m.st.mouse_sensitivity = sens;
        m.st.mouse_invert = inv;
        recomp_mouse_look_configure(look, sens, inv);
        settings_save();
    }
    ImGui::TextWrapped("With mouse look on, the game has the pointer while it is in front; "
                       "Esc gives it back.");
}

/* The picture: changed at once, on the frame behind the menu (the game is
 * not dimmed while this tab shows), and kept in the file on Apply or when
 * the menu closes. Per machine: online, nobody else sees it. */
void tab_picture()
{
    int b = m.st.picture_brightness, c = m.st.picture_contrast;
    int sat = m.st.picture_saturation, g = m.st.picture_gamma;
    bool changed = false;
    char label[32];

    ImGui::TextWrapped("Adjusted after the game, as a screen's own controls would be. "
                       "The game behind the menu shows each change.");
    ImGui::Spacing();
    snprintf(label, sizeof label, "%+d", b);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    changed |= ImGui::SliderInt("Brightness", &b, RECOMP_PICTURE_BRIGHTNESS_MIN,
                                RECOMP_PICTURE_BRIGHTNESS_MAX, b ? label : "0 (the game's)");
    snprintf(label, sizeof label, "%d%%%%", c);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    changed |= ImGui::SliderInt("Contrast", &c, RECOMP_PICTURE_CONTRAST_MIN,
                                RECOMP_PICTURE_CONTRAST_MAX, c != 100 ? label : "100%% (the game's)");
    snprintf(label, sizeof label, "%d%%%%", sat);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    changed |= ImGui::SliderInt("Saturation", &sat, RECOMP_PICTURE_SATURATION_MIN,
                                RECOMP_PICTURE_SATURATION_MAX, sat != 100 ? label : "100%% (the game's)");
    snprintf(label, sizeof label, "%.2f", (double)g / 100.0);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    changed |= ImGui::SliderInt("Gamma", &g, RECOMP_PICTURE_GAMMA_MIN, RECOMP_PICTURE_GAMMA_MAX,
                                g != 100 ? label : "1.00 (the game's)");
    bool neutral = b == 0 && c == 100 && sat == 100 && g == 100;
    if (neutral)
        ImGui::BeginDisabled();
    if (ImGui::Button("Reset to the game's")) {
        b = 0; c = 100; sat = 100; g = 100;
        changed = true;
    }
    if (neutral)
        ImGui::EndDisabled();
    if (changed) {
        m.st.picture_brightness = b;
        m.st.picture_contrast = c;
        m.st.picture_saturation = sat;
        m.st.picture_gamma = g;
        xbox_D3D8SetPicture(b, c, sat, g);
        m.unsaved = true;
        host_request_redraw();          /* the held frame again, with it */
    }
}

/* RECOMP_MENU_TEST's tab, selected on the menu's first frame. "Advanced" is
 * the Display tab with its Advanced part open. */
ImGuiTabItemFlags test_select(const char *tab)
{
    const char *want = !strcmp(m.test_tab, "Advanced") ? "Display" : m.test_tab;
    return want[0] && !strcmp(want, tab) ? ImGuiTabItemFlags_SetSelected : 0;
}

void draw_menu(unsigned width, unsigned height)
{
    const float em = ImGui::GetFontSize();
    const bool online = !g_held.load();
    float w = em * 30.0f;
    if (w > (float)width * 0.92f)
        w = (float)width * 0.92f;

    /* On the Picture tab the menu stands at the left, over the least of the
     * picture, and the game behind it is not dimmed. */
    if (m.picture_tab)
        ImGui::SetNextWindowPos(ImVec2(em * 1.5f, (float)height * 0.5f), ImGuiCond_Always,
                                ImVec2(0.0f, 0.5f));
    else
        ImGui::SetNextWindowPos(ImVec2((float)width * 0.5f, (float)height * 0.5f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);
    ImGui::Begin("##recomp_menu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(online ? "Menu" : "Paused");
    ImGui::SetWindowFontScale(1.0f);
    if (online) {
        ImGui::SameLine();
        ImGui::TextDisabled("  online: the game goes on");
    }
    ImGui::Spacing();

    float bw = (w - ImGui::GetStyle().WindowPadding.x * 2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Resume", ImVec2(bw, em * 2.0f)))
        recomp_menu_set_open(0);
    ImGui::SetItemDefaultFocus();       /* where the arrow keys start */
    ImGui::SameLine();
    if (ImGui::Button("Quit to desktop", ImVec2(bw, em * 2.0f)))
        m.confirm_quit = true;
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Display", nullptr, test_select("Display"))) {
            tab_display();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Frame rate", nullptr, test_select("Frame rate"))) {
            tab_frame_rate();
            ImGui::EndTabItem();
        }
        bool picture = false;
        if (ImGui::BeginTabItem("Picture", nullptr, test_select("Picture"))) {
            tab_picture();
            picture = true;
            ImGui::EndTabItem();
        }
        if (picture != m.picture_tab) {
            m.picture_tab = picture;
            host_request_redraw();      /* undimmed, or dimmed again */
        }
        if (ImGui::BeginTabItem("Mouse", nullptr, test_select("Mouse"))) {
            tab_mouse();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    /* Apply: what was changed here, kept in the settings file. Changes are
     * kept on leaving the menu as well; this says so, and does it now. */
    ImGui::Spacing();
    {
        float aw = w - ImGui::GetStyle().WindowPadding.x * 2;
        if (m.unsaved) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.62f, 0.16f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.72f, 0.30f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.86f, 0.52f, 0.10f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.03f, 1.00f));
            if (ImGui::Button("Apply  -  save these settings", ImVec2(aw, em * 2.2f)))
                settings_save_unsaved();
            ImGui::PopStyleColor(4);
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Saved", ImVec2(aw, em * 2.2f));
            ImGui::EndDisabled();
        }
    }

    if (restart_pending()) {
        ImGui::Separator();
        ImGui::TextWrapped("Some changes take effect when the game starts again.");
        if (!online && ImGui::Button("Restart now"))
            m.confirm_restart = true;
    }

    if (m.confirm_quit) {
        ImGui::OpenPopup("Quit to desktop?");
        m.confirm_quit = false;
    }
    if (m.confirm_restart) {
        ImGui::OpenPopup("Restart the game?");
        m.confirm_restart = false;
    }
    ImGui::SetNextWindowPos(ImVec2((float)width * 0.5f, (float)height * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit to desktop?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Anything since the game last saved is lost.");
        ImGui::Spacing();
        if (ImGui::Button("Quit", ImVec2(em * 7, 0))) {
            settings_save();
            xbox_HostExit("quit from the in-game menu");
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 7, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(ImVec2((float)width * 0.5f, (float)height * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Restart the game?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("The game starts again from its title screen.");
        ImGui::TextUnformatted("Anything since it last saved is lost.");
        ImGui::Spacing();
        if (ImGui::Button("Restart", ImVec2(em * 7, 0))) {
            char exe[1024];

            settings_save();
            if (host_exe_path(exe, sizeof exe))
                host_launch_other(exe);
            fprintf(stderr, "[MENU] could not start the game again\n");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 7, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

std::chrono::steady_clock::time_point g_last;

void menu_present(void *out_view, unsigned width, unsigned height)
{
    if (g_leaving.exchange(0)) {
        settings_save_unsaved();
        m.picture_tab = false;
    }
    /* RECOMP_MENU_TEST=<seconds>[:<tab>]: the menu opened that long after the
     * first frame, on that tab (Display, Advanced, Frame rate, Picture,
     * Mouse), presenting all the while, so a test run can take a picture of
     * it (RECOMP_PRESENT_DUMP) on any renderer. */
    {
        static int test = -1;
        static std::chrono::steady_clock::time_point start, at;
        if (test < 0) {
            const char *v = getenv("RECOMP_MENU_TEST");
            test = v && *v ? 1 : 0;
            start = std::chrono::steady_clock::now();
            if (test) {
                const char *colon = strchr(v, ':');
                at = start + std::chrono::milliseconds((long long)(atof(v) * 1000.0));
                snprintf(m.test_tab, sizeof m.test_tab, "%s", colon ? colon + 1 : "");
            }
        }
        if (test == 1 && std::chrono::steady_clock::now() >= at) {
            test = 2;
            m.test_on = true;
            fprintf(stderr, "[MENU] RECOMP_MENU_TEST: opened%s%s\n",
                    m.test_tab[0] ? " on " : "", m.test_tab);
            recomp_menu_set_open(1);
        }
    }
    if (!g_open.load() || !out_view || !width || !height)
        return;
    if (!gpu_create())
        return;
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        g_ctx = ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendRendererName = "recompose-rhi";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        make_base_style();
        g_last = std::chrono::steady_clock::now();
    }
    /* The file as it is now, each time the menu opens: the launcher or a
     * hand edit may have changed it since. */
    bool just_opened = g_reload.exchange(0) != 0;
    if (just_opened || !m.loaded)
        settings_load();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    float px = (float)height / 38.0f;
    px = px < 15.0f ? 15.0f : px > 44.0f ? 44.0f : (float)(int)px;
    if (!ensure_font(px))
        return;
    io.DisplaySize = ImVec2((float)width, (float)height);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last).count();
    g_last = now;
    io.DeltaTime = dt > 0.0001f ? (dt < 0.5f ? dt : 0.5f) : 0.0001f;
    /* A held title presents only when asked, and ImGui shows some results
     * of an input a frame later: a popup opened by a click (Quit to
     * desktop?) appears on the frame after, and the hover and active
     * colours settle over two. So each batch of input asks for two frames
     * more; without them, on a Mac, the confirmation only appeared at the
     * next key or move of the pointer. */
    static int settle;
    if (queue_drain(io) > 0 || just_opened)
        settle = 2;

    /* An auto-sized window knows its size only after a frame: laid out once
     * unseen when the menu has just opened, so it does not appear in the
     * wrong place for a frame while the title is held on a still picture. */
    if (just_opened || g_font_px != g_font_px_drawn) {
        ImGui::NewFrame();
        draw_menu(width, height);
        ImGui::EndFrame();
        g_font_px_drawn = g_font_px;
    }
    ImGui::NewFrame();
    /* A dimmed game behind the menu; not while the picture is being set. */
    if (!m.picture_tab)
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                      IM_COL32(0, 0, 0, 130));
    draw_menu(width, height);
    ImGui::Render();
    render(ImGui::GetDrawData(), (RhiView *)out_view, width, height);
    if (settle > 0 || m.test_on) {
        if (settle > 0)
            settle--;
        host_request_redraw();
    }
}

} // namespace

/* The window closed or the game quit with the menu's changes not saved yet. */
static void menu_save_at_exit(void)
{
    settings_save_unsaved();
}

extern "C" void recomp_menu_install(void)
{
    static bool done;

    if (done)
        return;
    done = true;
    atexit(menu_save_at_exit);
    xbox_D3D8SetUiHook(menu_present);
}
