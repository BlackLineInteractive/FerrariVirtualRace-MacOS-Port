// Direct3D 9 implemented on OpenGL 4.1 core (macOS).
//
// Rendering convention: everything is rendered upside-down into FBO textures so that row 0 of every
// GL texture is the top row in D3D terms. Uploaded textures therefore need no flipping, viewport and
// scissor Y coordinates are used as-is, and Present flips the back buffer once when blitting to the window.
#include "d3d9_internal.h"
#include "win32ui.h"
#include <mutex>
#include <unordered_map>
#include <pthread.h>
#include <cmath>
#include <cstring>
#include <algorithm>

// ====================================================================== locking / GL context
static std::recursive_mutex g_d3d_mutex;
static SDL_GLContext g_ctx;
static SDL_Window *g_win;
static pthread_t g_ctx_thread;
static bool g_ctx_bound;

void d3d_lock() {
    g_d3d_mutex.lock();
    if (g_ctx && (!g_ctx_bound || !pthread_equal(g_ctx_thread, pthread_self()))) {
        SDL_GL_MakeCurrent(g_win, g_ctx);
        g_ctx_thread = pthread_self();
        g_ctx_bound = true;
    }
}
void d3d_unlock() { g_d3d_mutex.unlock(); }

#define M(name) static void name(CPU *c)
#define GUARD D3DGuard guard_

static int g_d3d_trace = -1;
static bool trace() { if (g_d3d_trace < 0) g_d3d_trace = getenv("FVR_D3D_TRACE") ? 1 : 0; return g_d3d_trace; }

// ====================================================================== object registry
static std::unordered_map<uint32_t, Obj *> g_objs;

Obj *obj_get(uint32_t g) {
    if (!g) return nullptr;
    auto it = g_objs.find(g);
    return it == g_objs.end() ? nullptr : it->second;
}
uint32_t obj_publish(Obj *o, uint32_t vtable) {
    o->guest = com_new_object(vtable, 4);
    g_objs[o->guest] = o;
    return o->guest;
}
void obj_addref(Obj *o) {
    if (!o) return;
    if (o->kind == K_SURF && static_cast<Surface *>(o)->container) { obj_addref(static_cast<Surface *>(o)->container); return; }
    o->refs++;
}
uint32_t obj_release(Obj *o) {
    if (!o) return 0;
    if (o->kind == K_SURF && static_cast<Surface *>(o)->container) return obj_release(static_cast<Surface *>(o)->container);
    int r = --o->refs;
    if (r <= 0) {
        g_objs.erase(o->guest);
        gfree(o->guest);
        delete o;
        return 0;
    }
    return (uint32_t)r;
}

GLuint Surface::gl_tex() const { return container ? container->tex : tex; }
GLenum Surface::gl_target() const {
    if (container && container->kind == K_CUBE) return GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;
    return GL_TEXTURE_2D;
}

static void free_fbos_for(GLuint tex);
Surface::~Surface() {
    if (container) return;
    if (tex) { free_fbos_for(tex); glDeleteTextures(1, &tex); }
    if (mem) gfree(mem);
}
Texture::~Texture() {
    for (auto *s : surfs) if (s) { g_objs.erase(s->guest); gfree(s->guest); delete s; }
    for (auto m : mem) if (m) gfree(m);
    if (tex) { free_fbos_for(tex); glDeleteTextures(1, &tex); }
}
Buffer::~Buffer() {
    if (buf) glDeleteBuffers(1, &buf);
    if (mem) gfree(mem);
}

// ====================================================================== device globals
struct Program {
    GLuint prog = 0;
    bool ff_vs = false, ff_ps = false;
    GLint vc = -1, vi = -1, vb = -1, pc = -1, pi = -1, pb = -1, posFixup = -1, alphaFunc = -1, alphaRef = -1;
    GLint vfaceSign = -1, fogColor = -1, fogParams = -1;
    uint64_t vs_ver = ~0ull, ps_ver = ~0ull;
    Shader *vs = nullptr, *ps = nullptr;
};

struct StateBlock;
static struct Device {
    uint32_t guest = 0, d3d = 0, swap = 0;
    uint32_t hwnd = 0, behavior = 0;
    uint32_t pp[14] = {};
    DevState st;
    Surface *rt[4] = {}, *ds = nullptr;
    Surface *backbuf = nullptr, *autods = nullptr;
    GLuint vao = 0, up_vbo = 0, up_ibo = 0;
    GLuint samplers[20] = {};
    bool samp_dirty[20] = {};
    bool rs_dirty = true, fbo_dirty = true, vp_dirty = true;
    uint64_t vs_ver = 1, ps_ver = 1;
    std::map<std::string, Program> progs;
    std::map<std::string, GLuint> ff_shaders;
    StateBlock *recording = nullptr;
    GLuint cur_fbo = 0;
    uint32_t enabled_attribs = 0;
    bool in_scene = false;
    int cursor_shown = 0;
    uint16_t gamma[3][256];
    uint64_t frame = 0;
} *D;

static uint32_t g_vt[16];

// ====================================================================== D3DCAPS9
static void fill_caps(uint32_t p) {
    memset(H(p), 0, 304);
    auto w = [&](int off, uint32_t v) { W32(p + off, v); };
    auto f = [&](int off, float v) { WF(p + off, v); };
    w(0, 1); w(4, 0);
    w(8, 0x20000);                 // Caps: READ_SCANLINE
    w(12, 0xE0020000);             // Caps2: FULLSCREENGAMMA | CANAUTOGENMIPMAP | DYNAMICTEXTURES | CANMANAGERESOURCE
    w(16, 0x3A0);                  // Caps3
    w(20, 0x8000000F);             // PresentationIntervals
    w(24, 1);                      // CursorCaps: COLOR
    w(28, 0x001BBEF0);             // DevCaps
    w(32, 0x002FCEF2);             // PrimitiveMiscCaps
    w(36, 0x0F736191);             // RasterCaps
    w(40, 0xFF); w(44, 0x3FFF); w(48, 0x3FFF); w(52, 0xFF);
    w(56, 0x84208);                // ShadeCaps
    w(60, 0x0001EC45);             // TextureCaps (no POW2 restriction, mipmaps, cube, volume, projected)
    w(64, 0x07030700); w(68, 0x07030700); w(72, 0x03030300);
    w(76, 0x3F); w(80, 0x3F); w(84, 0x1F);
    w(88, 8192); w(92, 8192); w(96, 2048); w(100, 8192); w(104, 8192); w(108, 16);
    f(112, 1e10f); f(116, -1e8f); f(120, -1e8f); f(124, 1e8f); f(128, 1e8f); f(132, 0.0f);
    w(136, 0x1FF);                 // StencilCaps incl. TWOSIDED
    w(140, 0x80008);               // FVFCaps
    w(144, 0x03FFFFFF);            // TextureOpCaps
    w(148, 8); w(152, 8);
    w(156, 0x17B);                 // VertexProcessingCaps
    w(160, 8); w(164, 8); w(168, 4); w(172, 0);
    f(176, 8192.0f);
    w(180, 0xFFFFFF); w(184, 0xFFFFFF); w(188, 16); w(192, 508);
    w(196, 0xFFFE0300); w(200, 256); w(204, 0xFFFF0300); f(208, 65504.0f);
    w(212, 0x51); f(216, 0.0f); w(220, 0);
    w(224, 0); w(228, 0); w(232, 1);
    w(236, 0x30F);                 // DeclTypes
    w(240, 4);                     // NumSimultaneousRTs
    w(244, 0x03000300);
    w(248, 1); w(252, 24); w(256, 32); w(260, 4);           // VS20Caps
    w(264, 0x1F); w(268, 24); w(272, 32); w(276, 4); w(280, 512); // PS20Caps
    w(284, 0x03000300);
    w(288, 65535); w(292, 65535); w(296, 32768); w(300, 32768);
}

// ====================================================================== IUnknown / IDirect3DResource9 helpers
M(gen_QueryInterface) { GUARD; Obj *o = obj_get(ARG(0)); obj_addref(o); W32(ARG(2), ARG(0)); RETN(3, D3D_OK); }
M(gen_AddRef) { GUARD; Obj *o = obj_get(ARG(0)); obj_addref(o); RETN(1, o ? (o->kind == K_SURF && static_cast<Surface *>(o)->container ? static_cast<Surface *>(o)->container->refs : o->refs) : 0); }
M(gen_Release) {
    GUARD;
    Obj *o = obj_get(ARG(0));
    if (!o) RETN(1, 0);
    RETN(1, obj_release(o));
}
M(res_GetDevice) { GUARD; W32(ARG(1), D->guest); obj_addref(obj_get(D->guest)); RETN(2, D3D_OK); }
M(res_SetPrivateData) { RETN(5, D3D_OK); }
M(res_GetPrivateData) { RETN(4, D3DERR_NOTFOUND); }
M(res_FreePrivateData) { RETN(2, D3D_OK); }
M(res_SetPriority) { RETN(2, 0); }
M(res_GetPriority) { RETN(1, 0); }
M(res_PreLoad) { RETN(1, 0); }
static uint32_t res_type(Obj *o) {
    switch (o->kind) { case K_SURF: return 1; case K_TEX: return 3; case K_VOL: return 4; case K_CUBE: return 5; case K_VB: return 6; case K_IB: return 7; default: return 0; }
}
M(res_GetType) { GUARD; Obj *o = obj_get(ARG(0)); RETN(1, o ? res_type(o) : 0); }

// ====================================================================== textures
static GLenum tex_target(Kind k) { return k == K_CUBE ? GL_TEXTURE_CUBE_MAP : k == K_VOL ? GL_TEXTURE_3D : GL_TEXTURE_2D; }

static uint32_t mip_dim(uint32_t v, uint32_t l) { v >>= l; return v ? v : 1; }

static Texture *create_texture(Kind k, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    const FmtInfo *fi = fmt_info(fmt);
    if (!fi) { rtlog("d3d9: unsupported texture format %08x", fmt); return nullptr; }
    uint32_t maxdim = std::max(w, std::max(h, d));
    uint32_t full = 1;
    while ((maxdim >> full) > 0) full++;
    bool autogen = usage & 0x400;
    if (levels == 0 || levels > full) levels = full;
    if (autogen) levels = full;
    Texture *t = new Texture(k);
    t->w = w; t->h = h; t->d = d; t->levels = levels; t->fmt = fmt; t->usage = usage; t->pool = pool; t->autogen = autogen;
    int faces = k == K_CUBE ? 6 : 1;
    t->mem.assign(faces * levels, 0);
    t->surfs.assign(faces * levels, nullptr);
    if (t->sysmem()) return t;
    t->target = tex_target(k);
    glGenTextures(1, &t->tex);
    glBindTexture(t->target, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lw = mip_dim(w, l), lh = mip_dim(h, l), ld = mip_dim(d, l);
            GLenum tgt = k == K_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : t->target;
            if (fi->compressed) {
                uint32_t sz = fmt_pitch(fi, lw) * fmt_rows(fi, lh);
                std::vector<uint8_t> zero(sz * ld, 0);
                if (k == K_VOL) glCompressedTexImage3D(tgt, l, fi->internal, lw, lh, ld, 0, sz * ld, zero.data());
                else glCompressedTexImage2D(tgt, l, fi->internal, lw, lh, 0, sz, zero.data());
            } else if (k == K_VOL) glTexImage3D(tgt, l, fi->internal, lw, lh, ld, 0, fi->format, fi->type, nullptr);
            else glTexImage2D(tgt, l, fi->internal, lw, lh, 0, fi->format, fi->type, nullptr);
        }
    glTexParameteri(t->target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteriv(t->target, GL_TEXTURE_SWIZZLE_RGBA, fi->swz);
    return t;
}

static uint32_t level_size(Texture *t, uint32_t l, uint32_t *pitch, uint32_t *slice) {
    const FmtInfo *fi = fmt_info(t->fmt);
    uint32_t lw = mip_dim(t->w, l), lh = mip_dim(t->h, l), ld = t->kind == K_VOL ? mip_dim(t->d, l) : 1;
    uint32_t p = fmt_pitch(fi, lw), r = fmt_rows(fi, lh);
    if (pitch) *pitch = p;
    if (slice) *slice = p * r;
    return p * r * ld;
}

static void upload_level(Texture *t, int face, uint32_t l) {
    if (t->sysmem() || !t->tex) return;
    uint32_t m = t->mem[face * t->levels + l];
    if (!m) return;
    const FmtInfo *fi = fmt_info(t->fmt);
    uint32_t lw = mip_dim(t->w, l), lh = mip_dim(t->h, l), ld = t->kind == K_VOL ? mip_dim(t->d, l) : 1;
    uint32_t sz = level_size(t, l, nullptr, nullptr);
    glBindTexture(t->target, t->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    GLenum tgt = t->kind == K_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : t->target;
    if (fi->compressed) {
        if (t->kind == K_VOL) glCompressedTexSubImage3D(tgt, l, 0, 0, 0, lw, lh, ld, fi->internal, sz, H(m));
        else glCompressedTexSubImage2D(tgt, l, 0, 0, lw, lh, fi->internal, sz, H(m));
    } else {
        if (t->kind == K_VOL) glTexSubImage3D(tgt, l, 0, 0, 0, lw, lh, ld, fi->format, fi->type, H(m));
        else glTexSubImage2D(tgt, l, 0, 0, lw, lh, fi->format, fi->type, H(m));
    }
    if (t->autogen && l == 0) glGenerateMipmap(t->target);
}

static void readback_level(Texture *t, int face, uint32_t l) {
    uint32_t m = t->mem[face * t->levels + l];
    if (!m || !t->tex) return;
    const FmtInfo *fi = fmt_info(t->fmt);
    glBindTexture(t->target, t->tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    GLenum tgt = t->kind == K_CUBE ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : t->target;
    if (fi->compressed) glGetCompressedTexImage(tgt, l, H(m));
    else if (!fi->depth) glGetTexImage(tgt, l, fi->format, fi->type, H(m));
}

static uint32_t lock_level(Texture *t, int face, uint32_t l, uint32_t rect, uint32_t flags, uint32_t *pitch_out, uint32_t *slice_out) {
    uint32_t pitch, slice;
    uint32_t sz = level_size(t, l, &pitch, &slice);
    uint32_t &m = t->mem[face * t->levels + l];
    if (!m) {
        m = gmalloc(sz + 64, true);
        if (!(flags & 0x2000) && !(flags & 0x10)) readback_level(t, face, l);   // not DISCARD/READONLY-new
    }
    *pitch_out = pitch;
    if (slice_out) *slice_out = slice;
    uint32_t p = m;
    if (rect) {
        const FmtInfo *fi = fmt_info(t->fmt);
        uint32_t left = R32(rect), top = R32(rect + 4);
        if (fi->compressed) p += (top / 4) * pitch + (left / 4) * fi->bpp;
        else p += top * pitch + left * fi->bpp;
    }
    return p;
}

static void unlock_level(Texture *t, int face, uint32_t l) {
    upload_level(t, face, l);
    // Keep the CPU copy only where D3D would (dynamic / sysmem); otherwise free it to save guest memory.
    uint32_t &m = t->mem[face * t->levels + l];
    if (m && !t->sysmem() && !(t->usage & 0x200)) { gfree(m); m = 0; }
}

static Surface *tex_surface(Texture *t, int face, uint32_t l) {
    Surface *&s = t->surfs[face * t->levels + l];
    if (!s) {
        s = new Surface();
        s->container = t; s->level = (int)l; s->face = face;
        s->w = mip_dim(t->w, l); s->h = mip_dim(t->h, l); s->fmt = t->fmt; s->usage = t->usage; s->pool = t->pool;
        obj_publish(s, g_vt[K_SURF]);
    }
    return s;
}

static void write_surface_desc(uint32_t p, uint32_t fmt, uint32_t type, uint32_t usage, uint32_t pool, uint32_t ms, uint32_t w, uint32_t h) {
    W32(p, fmt); W32(p + 4, type); W32(p + 8, usage); W32(p + 12, pool); W32(p + 16, ms); W32(p + 20, 0); W32(p + 24, w); W32(p + 28, h);
}

M(tex_SetLOD) { GUARD; Texture *t = OBJ<Texture>(ARG(0)); uint32_t old = t->lod; t->lod = std::min(ARG(1), t->levels - 1);
    if (t->tex) { glBindTexture(t->target, t->tex); glTexParameteri(t->target, GL_TEXTURE_BASE_LEVEL, t->lod); } RETN(2, old); }
M(tex_GetLOD) { GUARD; RETN(1, OBJ<Texture>(ARG(0))->lod); }
M(tex_GetLevelCount) { GUARD; Texture *t = OBJ<Texture>(ARG(0)); RETN(1, t->autogen ? 1 : t->levels); }
M(tex_SetAutoGenFilterType) { RETN(2, D3D_OK); }
M(tex_GetAutoGenFilterType) { RETN(1, 2); }
M(tex_GenerateMipSubLevels) { GUARD; Texture *t = OBJ<Texture>(ARG(0)); if (t->tex) { glBindTexture(t->target, t->tex); glGenerateMipmap(t->target); } RETN(1, 0); }
M(tex_GetLevelDesc) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    uint32_t l = ARG(1);
    if (l >= t->levels) RETN(3, D3DERR_INVALIDCALL);
    write_surface_desc(ARG(2), t->fmt, 1, t->usage, t->pool, 0, mip_dim(t->w, l), mip_dim(t->h, l));
    RETN(3, D3D_OK);
}
M(tex_GetSurfaceLevel) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    if (ARG(1) >= t->levels) RETN(3, D3DERR_INVALIDCALL);
    Surface *s = tex_surface(t, 0, ARG(1));
    obj_addref(s);
    W32(ARG(2), s->guest);
    RETN(3, D3D_OK);
}
M(tex_LockRect) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    uint32_t l = ARG(1);
    if (l >= t->levels) RETN(5, D3DERR_INVALIDCALL);
    uint32_t pitch;
    uint32_t p = lock_level(t, 0, l, ARG(3), ARG(4), &pitch, nullptr);
    W32(ARG(2), pitch); W32(ARG(2) + 4, p);
    RETN(5, D3D_OK);
}
M(tex_UnlockRect) { GUARD; Texture *t = OBJ<Texture>(ARG(0)); if (ARG(1) < t->levels) unlock_level(t, 0, ARG(1)); RETN(2, D3D_OK); }
M(tex_AddDirtyRect) { RETN(2, D3D_OK); }

M(cube_GetLevelDesc) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    uint32_t l = ARG(1);
    write_surface_desc(ARG(2), t->fmt, 1, t->usage, t->pool, 0, mip_dim(t->w, l), mip_dim(t->h, l));
    RETN(3, D3D_OK);
}
M(cube_GetCubeMapSurface) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    if (ARG(1) > 5 || ARG(2) >= t->levels) RETN(4, D3DERR_INVALIDCALL);
    Surface *s = tex_surface(t, (int)ARG(1), ARG(2));
    obj_addref(s);
    W32(ARG(3), s->guest);
    RETN(4, D3D_OK);
}
M(cube_LockRect) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    uint32_t pitch;
    uint32_t p = lock_level(t, (int)ARG(1), ARG(2), ARG(4), ARG(5), &pitch, nullptr);
    W32(ARG(3), pitch); W32(ARG(3) + 4, p);
    RETN(6, D3D_OK);
}
M(cube_UnlockRect) { GUARD; unlock_level(OBJ<Texture>(ARG(0)), (int)ARG(1), ARG(2)); RETN(3, D3D_OK); }
M(cube_AddDirtyRect) { RETN(3, D3D_OK); }

M(vol_GetLevelDesc) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    uint32_t l = ARG(1), p = ARG(2);
    W32(p, t->fmt); W32(p + 4, 2); W32(p + 8, t->usage); W32(p + 12, t->pool);
    W32(p + 16, mip_dim(t->w, l)); W32(p + 20, mip_dim(t->h, l)); W32(p + 24, mip_dim(t->d, l));
    RETN(3, D3D_OK);
}
M(vol_LockBox) {
    GUARD; Texture *t = OBJ<Texture>(ARG(0));
    uint32_t pitch, slice;
    uint32_t p = lock_level(t, 0, ARG(1), 0, ARG(4), &pitch, &slice);
    if (ARG(3)) { uint32_t b = ARG(3); p += R32(b + 16) * slice + R32(b + 4) * pitch + R32(b) * fmt_info(t->fmt)->bpp; }
    W32(ARG(2), pitch); W32(ARG(2) + 4, slice); W32(ARG(2) + 8, p);
    RETN(5, D3D_OK);
}
M(vol_UnlockBox) { GUARD; unlock_level(OBJ<Texture>(ARG(0)), 0, ARG(1)); RETN(2, D3D_OK); }

// ====================================================================== surfaces
static Surface *create_surface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, uint32_t ms) {
    const FmtInfo *fi = fmt_info(fmt);
    if (!fi) { rtlog("d3d9: unsupported surface format %08x", fmt); return nullptr; }
    Surface *s = new Surface();
    s->w = w; s->h = h; s->fmt = fmt; s->usage = usage; s->pool = pool; s->ms = ms;
    if (pool == 2 || pool == 3) {   // system memory surface
        s->mem = gmalloc(fmt_pitch(fi, w) * fmt_rows(fi, h) + 64, true);
    } else {
        glGenTextures(1, &s->tex);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        if (fi->compressed) {
            uint32_t sz = fmt_pitch(fi, w) * fmt_rows(fi, h);
            std::vector<uint8_t> z(sz, 0);
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, fi->internal, w, h, 0, sz, z.data());
        } else glTexImage2D(GL_TEXTURE_2D, 0, fi->internal, w, h, 0, fi->format, fi->type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    return s;
}
static void destroy_surface_gl(Surface *s) {
    if (s->tex) { free_fbos_for(s->tex); glDeleteTextures(1, &s->tex); s->tex = 0; }
    if (s->mem) { gfree(s->mem); s->mem = 0; }
}
struct SurfaceDeleter;

M(surf_GetContainer) {
    GUARD; Surface *s = OBJ<Surface>(ARG(0));
    if (s->container) { obj_addref(s->container); W32(ARG(2), s->container->guest); RETN(3, D3D_OK); }
    W32(ARG(2), D->guest); obj_addref(obj_get(D->guest));
    RETN(3, D3D_OK);
}
M(surf_GetDesc) {
    GUARD; Surface *s = OBJ<Surface>(ARG(0));
    uint32_t usage = s->usage;
    write_surface_desc(ARG(1), s->fmt, 1, usage, s->pool, s->ms, s->w, s->h);
    RETN(2, D3D_OK);
}
static uint32_t surf_mem(Surface *s, uint32_t *pitch, bool read) {
    const FmtInfo *fi = fmt_info(s->fmt);
    *pitch = fmt_pitch(fi, s->w);
    if (s->container) {
        uint32_t p;
        return lock_level(s->container, s->face, s->level, 0, read ? 0 : 0x2000, &p, nullptr);
    }
    if (!s->mem) {
        s->mem = gmalloc(*pitch * fmt_rows(fi, s->h) + 64, true);
        if (read && s->tex && !fi->depth && !fi->compressed) {
            glBindTexture(GL_TEXTURE_2D, s->tex);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, fi->format, fi->type, H(s->mem));
        }
    }
    return s->mem;
}
M(surf_LockRect) {
    GUARD; Surface *s = OBJ<Surface>(ARG(0));
    uint32_t pitch;
    uint32_t m = surf_mem(s, &pitch, !(ARG(3) & 0x2000));
    if (ARG(2)) {
        const FmtInfo *fi = fmt_info(s->fmt);
        uint32_t left = R32(ARG(2)), top = R32(ARG(2) + 4);
        m += fi->compressed ? (top / 4) * pitch + (left / 4) * fi->bpp : top * pitch + left * fi->bpp;
    }
    s->locked = true;
    W32(ARG(1), pitch); W32(ARG(1) + 4, m);
    RETN(4, D3D_OK);
}
M(surf_UnlockRect) {
    GUARD; Surface *s = OBJ<Surface>(ARG(0));
    s->locked = false;
    if (s->container) unlock_level(s->container, s->face, s->level);
    else if (s->tex && s->mem) {
        const FmtInfo *fi = fmt_info(s->fmt);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (fi->compressed) glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, fi->internal, fmt_pitch(fi, s->w) * fmt_rows(fi, s->h), H(s->mem));
        else if (!fi->depth) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, fi->format, fi->type, H(s->mem));
        gfree(s->mem); s->mem = 0;
    }
    RETN(1, D3D_OK);
}
M(surf_GetDC) { RETN(2, D3DERR_INVALIDCALL); }
M(surf_ReleaseDC) { RETN(2, D3D_OK); }

// ====================================================================== vertex / index buffers
static Buffer *create_buffer(Kind k, uint32_t len, uint32_t usage, uint32_t fmt_or_fvf, uint32_t pool) {
    Buffer *b = new Buffer(k);
    b->size = len; b->usage = usage; b->pool = pool;
    if (k == K_VB) b->fvf = fmt_or_fvf; else b->fmt = fmt_or_fvf;
    b->mem = gmalloc(len + 16, true);
    glGenBuffers(1, &b->buf);
    glBindBuffer(GL_ARRAY_BUFFER, b->buf);
    glBufferData(GL_ARRAY_BUFFER, len, nullptr, (usage & 0x200) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    return b;
}
M(buf_Lock) {
    GUARD; Buffer *b = OBJ<Buffer>(ARG(0));
    uint32_t off = ARG(1), size = ARG(2), flags = ARG(4);
    if (off > b->size) off = b->size;
    if (!size || off + size > b->size) size = b->size - off;
    if (!(flags & 0x10)) {     // not READONLY: remember the dirty range
        b->dirty_lo = std::min(b->dirty_lo, off);
        b->dirty_hi = std::max(b->dirty_hi, off + size);
    }
    b->lock_flags |= flags;
    b->locks++;
    W32(ARG(3), b->mem + off);
    RETN(5, D3D_OK);
}
static void buffer_flush(Buffer *b) {
    if (b->dirty_lo >= b->dirty_hi) return;
    glBindBuffer(GL_ARRAY_BUFFER, b->buf);
    if ((b->lock_flags & 0x2000) || (b->dirty_lo == 0 && b->dirty_hi == b->size)) {   // DISCARD / whole buffer: orphan
        glBufferData(GL_ARRAY_BUFFER, b->size, nullptr, (b->usage & 0x200) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, b->size, H(b->mem));
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, b->dirty_lo, b->dirty_hi - b->dirty_lo, H(b->mem + b->dirty_lo));
    }
    b->dirty_lo = ~0u; b->dirty_hi = 0;
    b->lock_flags = 0;
}
M(buf_Unlock) {
    GUARD; Buffer *b = OBJ<Buffer>(ARG(0));
    if (b->locks > 0 && --b->locks == 0) buffer_flush(b);
    RETN(1, D3D_OK);
}
M(vb_GetDesc) {
    GUARD; Buffer *b = OBJ<Buffer>(ARG(0)); uint32_t p = ARG(1);
    W32(p, 100); W32(p + 4, 6); W32(p + 8, b->usage); W32(p + 12, b->pool); W32(p + 16, b->size); W32(p + 20, b->fvf);
    RETN(2, D3D_OK);
}
M(ib_GetDesc) {
    GUARD; Buffer *b = OBJ<Buffer>(ARG(0)); uint32_t p = ARG(1);
    W32(p, b->fmt); W32(p + 4, 7); W32(p + 8, b->usage); W32(p + 12, b->pool); W32(p + 16, b->size);
    RETN(2, D3D_OK);
}

// ====================================================================== declarations / shaders / queries
M(decl_GetDeclaration) {
    GUARD; Decl *d = OBJ<Decl>(ARG(0));
    uint32_t n = (uint32_t)d->el.size() + 1;
    if (ARG(1)) {
        for (size_t i = 0; i < d->el.size(); i++) memcpy(H(ARG(1) + 8 * i), &d->el[i], 8);
        uint32_t e = ARG(1) + 8 * (uint32_t)d->el.size();
        W16(e, 0xFF); W16(e + 2, 0); W8(e + 4, 17); W8(e + 5, 0); W8(e + 6, 0); W8(e + 7, 0);
    }
    W32(ARG(2), n);
    RETN(3, D3D_OK);
}
M(sh_GetFunction) {
    GUARD; Shader *s = OBJ<Shader>(ARG(0));
    uint32_t sz = (uint32_t)s->code.size() * 4;
    if (ARG(1)) memcpy(H(ARG(1)), s->code.data(), std::min(sz, R32(ARG(2))));
    W32(ARG(2), sz);
    RETN(3, D3D_OK);
}
M(q_GetType) { GUARD; RETN(1, OBJ<Query>(ARG(0))->type); }
M(q_GetDataSize) { GUARD; Query *q = OBJ<Query>(ARG(0)); RETN(1, q->type == 8 ? 4 : q->type == 9 ? 4 : 8); }
M(q_Issue) {
    GUARD; Query *q = OBJ<Query>(ARG(0));
    uint32_t fl = ARG(1);
    if (q->type == 9) {
        if (!q->q) glGenQueries(1, &q->q);
        if (fl & 2) glBeginQuery(GL_SAMPLES_PASSED, q->q);   // BEGIN
        if (fl & 1) glEndQuery(GL_SAMPLES_PASSED);           // END
    }
    q->issued = true;
    RETN(2, D3D_OK);
}
M(q_GetData) {
    GUARD; Query *q = OBJ<Query>(ARG(0));
    uint32_t p = ARG(1), sz = ARG(2);
    if (q->type == 9 && q->q) {
        GLuint avail = 0;
        glGetQueryObjectuiv(q->q, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail && !(ARG(3) & 1)) RETN(4, S_FALSE);
        GLuint r = 0; glGetQueryObjectuiv(q->q, GL_QUERY_RESULT, &r);
        if (p && sz >= 4) W32(p, r);
        RETN(4, D3D_OK);
    }
    if (p && sz >= 4) W32(p, 1);
    RETN(4, D3D_OK);
}

// ====================================================================== state blocks
struct StateBlock : Obj {
    StateBlock() : Obj(K_SB) {}
    DevState s;
    bool all = false;
    std::vector<bool> rs = std::vector<bool>(256), tss = std::vector<bool>(8 * 33), samp = std::vector<bool>(20 * 14);
    std::vector<bool> tex = std::vector<bool>(20), xf = std::vector<bool>(512), vsf = std::vector<bool>(256), psf = std::vector<bool>(224);
    bool vsi = false, psi = false, vsb = false, psb = false, vp = false, scissor = false, streams = false, ib = false;
    bool decl = false, vs = false, ps = false, mat = false, lights = false, clip = false;
};
static void sb_mark_all(StateBlock *b, uint32_t type) {
    // type: 1 ALL, 2 PIXELSTATE, 3 VERTEXSTATE (approximate split)
    b->all = type == 1;
    std::fill(b->rs.begin(), b->rs.end(), true);
    std::fill(b->tss.begin(), b->tss.end(), true);
    std::fill(b->samp.begin(), b->samp.end(), true);
    std::fill(b->vsf.begin(), b->vsf.end(), type != 2);
    std::fill(b->psf.begin(), b->psf.end(), type != 3);
    if (type == 1) { std::fill(b->tex.begin(), b->tex.end(), true); std::fill(b->xf.begin(), b->xf.end(), true); }
    b->vsi = b->vsb = type != 2; b->psi = b->psb = type != 3;
    b->vs = type != 2; b->ps = type != 3; b->decl = type != 2;
    b->vp = b->scissor = b->streams = b->ib = b->mat = b->lights = b->clip = type == 1;
}
static void sb_capture(StateBlock *b) {
    DevState &d = D->st, &s = b->s;
    for (int i = 0; i < 256; i++) if (b->rs[i]) s.rs[i] = d.rs[i];
    for (int i = 0; i < 8 * 33; i++) if (b->tss[i]) s.tss[i / 33][i % 33] = d.tss[i / 33][i % 33];
    for (int i = 0; i < 20 * 14; i++) if (b->samp[i]) s.samp[i / 14][i % 14] = d.samp[i / 14][i % 14];
    for (int i = 0; i < 20; i++) if (b->tex[i]) s.tex[i] = d.tex[i];
    for (int i = 0; i < 512; i++) if (b->xf[i]) memcpy(s.transform[i], d.transform[i], 64);
    for (int i = 0; i < 256; i++) if (b->vsf[i]) memcpy(s.vsf[i], d.vsf[i], 16);
    for (int i = 0; i < 224; i++) if (b->psf[i]) memcpy(s.psf[i], d.psf[i], 16);
    if (b->vsi) memcpy(s.vsi, d.vsi, sizeof s.vsi);
    if (b->psi) memcpy(s.psi, d.psi, sizeof s.psi);
    if (b->vsb) memcpy(s.vsb, d.vsb, sizeof s.vsb);
    if (b->psb) memcpy(s.psb, d.psb, sizeof s.psb);
    if (b->vp) s.vp = d.vp;
    if (b->scissor) memcpy(s.scissor, d.scissor, sizeof s.scissor);
    if (b->streams) memcpy(s.streams, d.streams, sizeof s.streams);
    if (b->ib) s.ib = d.ib;
    if (b->decl) { s.decl = d.decl; s.fvf = d.fvf; }
    if (b->vs) s.vs = d.vs;
    if (b->ps) s.ps = d.ps;
    if (b->mat) s.mat = d.mat;
    if (b->lights) { memcpy(s.lights, d.lights, sizeof s.lights); memcpy(s.light_on, d.light_on, sizeof s.light_on); }
    if (b->clip) memcpy(s.clip, d.clip, sizeof s.clip);
}
static void set_texture(int stage, Obj *t);
static void set_stream(int i, Buffer *vb, uint32_t off, uint32_t stride);
static void set_indices(Buffer *ib);
static void set_shader(bool ps, Shader *s);
static void set_decl(Decl *d);
static void sb_apply(StateBlock *b) {
    DevState &d = D->st, &s = b->s;
    for (int i = 0; i < 256; i++) if (b->rs[i]) d.rs[i] = s.rs[i];
    for (int i = 0; i < 8 * 33; i++) if (b->tss[i]) d.tss[i / 33][i % 33] = s.tss[i / 33][i % 33];
    for (int i = 0; i < 20 * 14; i++) if (b->samp[i]) { d.samp[i / 14][i % 14] = s.samp[i / 14][i % 14]; D->samp_dirty[i / 14] = true; }
    for (int i = 0; i < 20; i++) if (b->tex[i]) set_texture(i, s.tex[i]);
    for (int i = 0; i < 512; i++) if (b->xf[i]) memcpy(d.transform[i], s.transform[i], 64);
    for (int i = 0; i < 256; i++) if (b->vsf[i]) { memcpy(d.vsf[i], s.vsf[i], 16); D->vs_ver++; }
    for (int i = 0; i < 224; i++) if (b->psf[i]) { memcpy(d.psf[i], s.psf[i], 16); D->ps_ver++; }
    if (b->vsi) { memcpy(d.vsi, s.vsi, sizeof s.vsi); D->vs_ver++; }
    if (b->psi) { memcpy(d.psi, s.psi, sizeof s.psi); D->ps_ver++; }
    if (b->vsb) { memcpy(d.vsb, s.vsb, sizeof s.vsb); D->vs_ver++; }
    if (b->psb) { memcpy(d.psb, s.psb, sizeof s.psb); D->ps_ver++; }
    if (b->vp) { d.vp = s.vp; D->vp_dirty = true; }
    if (b->scissor) memcpy(d.scissor, s.scissor, sizeof s.scissor);
    if (b->streams) for (int i = 0; i < 16; i++) { set_stream(i, s.streams[i].vb, s.streams[i].offset, s.streams[i].stride); d.streams[i].freq = s.streams[i].freq; }
    if (b->ib) set_indices(s.ib);
    if (b->decl) { set_decl(s.decl); d.fvf = s.fvf; }
    if (b->vs) set_shader(false, s.vs);
    if (b->ps) set_shader(true, s.ps);
    if (b->mat) d.mat = s.mat;
    if (b->lights) { memcpy(d.lights, s.lights, sizeof s.lights); memcpy(d.light_on, s.light_on, sizeof s.light_on); }
    if (b->clip) memcpy(d.clip, s.clip, sizeof s.clip);
    D->rs_dirty = true;
}
M(sb_Capture) { GUARD; sb_capture(OBJ<StateBlock>(ARG(0))); RETN(1, D3D_OK); }
M(sb_Apply) { GUARD; sb_apply(OBJ<StateBlock>(ARG(0))); RETN(1, D3D_OK); }

// ====================================================================== FBO cache
static std::map<std::string, GLuint> g_fbos;
static void free_fbos_for(GLuint tex) {
    std::string needle = "|" + std::to_string(tex) + ":";
    for (auto it = g_fbos.begin(); it != g_fbos.end();) {
        if (("|" + it->first).find(needle) != std::string::npos) {
            if (D && D->cur_fbo == it->second) { glBindFramebuffer(GL_FRAMEBUFFER, 0); D->cur_fbo = 0; D->fbo_dirty = true; }
            glDeleteFramebuffers(1, &it->second);
            it = g_fbos.erase(it);
        } else ++it;
    }
}
static void attach(GLenum point, Surface *s) {
    if (!s) { glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_2D, 0, 0); return; }
    glFramebufferTexture2D(GL_FRAMEBUFFER, point, s->gl_target(), s->gl_tex(), s->container ? s->level : 0);
}
static GLuint fbo_for(Surface *const *rts, int nrt, Surface *ds) {
    std::string key;
    for (int i = 0; i < nrt; i++) key += rts[i] ? std::to_string(rts[i]->gl_tex()) + ":" + std::to_string(rts[i]->gl_target()) + "." + std::to_string(rts[i]->level) + "|" : "0:|";
    key += ds ? std::to_string(ds->gl_tex()) + ":" + std::to_string(ds->level) + "|" : "0:|";
    auto it = g_fbos.find(key);
    if (it != g_fbos.end()) return it->second;
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    GLenum bufs[4];
    int nb = 0;
    for (int i = 0; i < nrt; i++) {
        attach(GL_COLOR_ATTACHMENT0 + i, rts[i]);
        bufs[i] = rts[i] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        if (rts[i]) nb = i + 1;
    }
    if (ds) {
        const FmtInfo *fi = fmt_info(ds->fmt);
        attach(fi && fi->stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, ds);
    }
    if (nb) glDrawBuffers(nb, bufs); else glDrawBuffer(GL_NONE);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) rtlog("d3d9: FBO incomplete %04x (%s)", st, key.c_str());
    g_fbos[key] = f;
    D->cur_fbo = f;
    return f;
}
static void bind_rt_fbo() {
    if (!D->fbo_dirty) return;
    GLuint f = fbo_for(D->rt, 4, D->ds);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    D->cur_fbo = f;
    D->fbo_dirty = false;
}

// ====================================================================== state application
static const GLenum BLEND[16] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR};
static GLenum blendf(uint32_t v) { return v < 16 ? BLEND[v] : GL_ONE; }
static GLenum blendop(uint32_t v) {
    switch (v) { case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX; default: return GL_FUNC_ADD; }
}
static GLenum cmpf(uint32_t v) { return v >= 1 && v <= 8 ? GL_NEVER + (v - 1) : GL_ALWAYS; }
static GLenum stencilop(uint32_t v) {
    switch (v) { case 2: return GL_ZERO; case 3: return GL_REPLACE; case 4: return GL_INCR; case 5: return GL_DECR; case 6: return GL_INVERT; case 7: return GL_INCR_WRAP; case 8: return GL_DECR_WRAP; default: return GL_KEEP; }
}
static float fl(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static void apply_viewport() {
    Viewport &v = D->st.vp;
    glViewport(v.x, v.y, v.w, v.h);
    glDepthRange(v.minz, v.maxz);
    D->vp_dirty = false;
}

static void apply_render_state() {
    uint32_t *rs = D->st.rs;
    // depth
    if (rs[7] && D->ds) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmpf(rs[23])); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[14] && D->ds ? GL_TRUE : GL_FALSE);
    // cull (Y-flipped rendering mirrors the winding)
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK); }
    glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
    // blending
    if (rs[27]) {
        glEnable(GL_BLEND);
        uint32_t sb = rs[19], db = rs[20];
        if (sb == 12) { sb = 5; db = 6; } else if (sb == 13) { sb = 6; db = 5; }
        if (rs[206]) {
            glBlendFuncSeparate(blendf(sb), blendf(db), blendf(rs[207]), blendf(rs[208]));
            glBlendEquationSeparate(blendop(rs[171]), blendop(rs[209]));
        } else {
            glBlendFunc(blendf(sb), blendf(db));
            glBlendEquation(blendop(rs[171]));
        }
        uint32_t bf = rs[193];
        glBlendColor(((bf >> 16) & 255) / 255.f, ((bf >> 8) & 255) / 255.f, (bf & 255) / 255.f, (bf >> 24) / 255.f);
    } else glDisable(GL_BLEND);
    uint32_t cw = rs[168];
    glColorMaski(0, cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    for (int i = 1; i < 4; i++) { uint32_t m = rs[189 + i]; glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1); }
    // stencil
    if (rs[52] && D->ds) {
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(rs[185] ? GL_FRONT : GL_FRONT_AND_BACK, cmpf(rs[56]), rs[57], rs[58]);
        glStencilOpSeparate(rs[185] ? GL_FRONT : GL_FRONT_AND_BACK, stencilop(rs[53]), stencilop(rs[54]), stencilop(rs[55]));
        if (rs[185]) {
            glStencilFuncSeparate(GL_BACK, cmpf(rs[189]), rs[57], rs[58]);
            glStencilOpSeparate(GL_BACK, stencilop(rs[186]), stencilop(rs[187]), stencilop(rs[188]));
        }
        glStencilMask(rs[59]);
    } else { glDisable(GL_STENCIL_TEST); glStencilMask(0xFFFFFFFF); }
    // scissor
    if (rs[174]) {
        glEnable(GL_SCISSOR_TEST);
        int *s = D->st.scissor;
        glScissor(s[0], s[1], std::max(0, s[2] - s[0]), std::max(0, s[3] - s[1]));
    } else glDisable(GL_SCISSOR_TEST);
    // depth bias
    float bias = fl(rs[195]), slope = fl(rs[175]);
    if (bias != 0.0f || slope != 0.0f) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(slope, bias * 16777215.0f); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    if (rs[194]) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
    // user clip planes are handled through gl_ClipDistance only in FF; ignore for shaders
    if (rs[156]) glEnable(GL_PROGRAM_POINT_SIZE);
    D->rs_dirty = false;
}

static GLenum addr_mode(uint32_t v) {
    switch (v) { case 2: return GL_MIRRORED_REPEAT; case 3: return GL_CLAMP_TO_EDGE; case 4: return GL_CLAMP_TO_BORDER; case 5: return GL_MIRRORED_REPEAT; default: return GL_REPEAT; }
}
static void apply_sampler(int i) {
    uint32_t *s = D->st.samp[i];
    GLuint sm = D->samplers[i];
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, addr_mode(s[1]));
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, addr_mode(s[2]));
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_R, addr_mode(s[3]));
    uint32_t bc = s[4];
    float col[4] = {((bc >> 16) & 255) / 255.f, ((bc >> 8) & 255) / 255.f, (bc & 255) / 255.f, (bc >> 24) / 255.f};
    glSamplerParameterfv(sm, GL_TEXTURE_BORDER_COLOR, col);
    uint32_t mag = s[5], min = s[6], mip = s[7];
    glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, mag == 1 || mag == 0 ? GL_NEAREST : GL_LINEAR);
    GLenum mf;
    bool lin = !(min == 1 || min == 0);
    if (mip == 0) mf = lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) mf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else mf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, mf);
    float aniso = (min == 3 || mag == 3) ? (float)std::max<uint32_t>(1, std::min<uint32_t>(16, s[10])) : 1.0f;
    glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
    glSamplerParameterf(sm, GL_TEXTURE_LOD_BIAS, fl(s[8]));
    glSamplerParameterf(sm, GL_TEXTURE_MIN_LOD, (float)s[9]);
    D->samp_dirty[i] = false;
}

// ====================================================================== programs
static GLuint compile_shader(GLenum type, const std::string &src) {
    GLuint sh = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        rtlog("d3d9: shader compile failed:\n%s\n----\n%s", log, src.c_str());
    }
    return sh;
}

static Program *get_program(Decl *decl) {
    DevState &st = D->st;
    Shader *vs = st.vs && st.vs->ok ? st.vs : nullptr;
    Shader *ps = st.ps && st.ps->ok ? st.ps : nullptr;
    std::string key = std::to_string((uintptr_t)vs) + "/" + std::to_string((uintptr_t)ps);
    std::string vsrc, psrc, k1, k2;
    SmShader ffps_if, ffvs_if;
    if (!vs) {
        ffps_if = ps ? ps->sm : ff_ps_iface();
        vsrc = ff_vs_source(st, decl, &ffps_if, k1);
        key += "|" + k1;
    }
    if (!ps) {
        psrc = ff_ps_source(st, vs ? &vs->sm : nullptr, k2);
        key += "|" + k2;
    }
    auto it = D->progs.find(key);
    if (it != D->progs.end()) return &it->second;
    if (vs) { ffps_if = ff_ps_iface(); vsrc = sm_vs_source(vs->sm, ps ? &ps->sm : &ffps_if); }
    if (ps) { ffvs_if = ff_vs_iface(decl, st); psrc = sm_ps_source(ps->sm, vs ? &vs->sm : &ffvs_if); }
    Program &p = D->progs[key];
    p.vs = vs; p.ps = ps; p.ff_vs = !vs; p.ff_ps = !ps;
    GLuint a = compile_shader(GL_VERTEX_SHADER, vsrc), b = compile_shader(GL_FRAGMENT_SHADER, psrc);
    p.prog = glCreateProgram();
    glAttachShader(p.prog, a); glAttachShader(p.prog, b);
    glLinkProgram(p.prog);
    GLint ok = 0; glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetProgramInfoLog(p.prog, sizeof log, nullptr, log);
        rtlog("d3d9: program link failed: %s\nVS:\n%s\nPS:\n%s", log, vsrc.c_str(), psrc.c_str());
    }
    glDeleteShader(a); glDeleteShader(b);
    glUseProgram(p.prog);
    auto U = [&](const char *n) { return glGetUniformLocation(p.prog, n); };
    p.vc = U("vc"); p.vi = U("vi"); p.vb = U("vb"); p.pc = U("pc"); p.pi = U("pi"); p.pb = U("pb");
    p.posFixup = U("posFixup"); p.alphaFunc = U("alphaFunc"); p.alphaRef = U("alphaRef"); p.vfaceSign = U("vfaceSign");
    p.fogColor = U("fogColor"); p.fogParams = U("fogParams");
    char nm[16];
    for (int i = 0; i < 16; i++) { snprintf(nm, sizeof nm, "ps_s%d", i); GLint l = U(nm); if (l >= 0) glUniform1i(l, i); }
    for (int i = 0; i < 4; i++) { snprintf(nm, sizeof nm, "vs_s%d", i); GLint l = U(nm); if (l >= 0) glUniform1i(l, 16 + i); }
    return &p;
}

static void upload_consts(Program *p) {
    DevState &st = D->st;
    if (p->vs && p->vs_ver != D->vs_ver) {
        float buf[256][4];
        memcpy(buf, st.vsf, sizeof buf);
        for (auto &d : p->vs->sm.fdefs) if (d.first < 256) memcpy(buf[d.first], d.second.data(), 16);
        if (p->vc >= 0) glUniform4fv(p->vc, 256, &buf[0][0]);
        int ib[16][4]; memcpy(ib, st.vsi, sizeof ib);
        for (auto &d : p->vs->sm.idefs) if (d.first < 16) memcpy(ib[d.first], d.second.data(), 16);
        if (p->vi >= 0) glUniform4iv(p->vi, 16, &ib[0][0]);
        int bb[16]; memcpy(bb, st.vsb, sizeof bb);
        for (auto &d : p->vs->sm.bdefs) if (d.first < 16) bb[d.first] = d.second;
        if (p->vb >= 0) glUniform1iv(p->vb, 16, bb);
        p->vs_ver = D->vs_ver;
    }
    if (p->ps && p->ps_ver != D->ps_ver) {
        float buf[224][4];
        memcpy(buf, st.psf, sizeof buf);
        for (auto &d : p->ps->sm.fdefs) if (d.first < 224) memcpy(buf[d.first], d.second.data(), 16);
        if (p->pc >= 0) glUniform4fv(p->pc, 224, &buf[0][0]);
        int ib[16][4]; memcpy(ib, st.psi, sizeof ib);
        for (auto &d : p->ps->sm.idefs) if (d.first < 16) memcpy(ib[d.first], d.second.data(), 16);
        if (p->pi >= 0) glUniform4iv(p->pi, 16, &ib[0][0]);
        int bb[16]; memcpy(bb, st.psb, sizeof bb);
        for (auto &d : p->ps->sm.bdefs) if (d.first < 16) bb[d.first] = d.second;
        if (p->pb >= 0) glUniform1iv(p->pb, 16, bb);
        p->ps_ver = D->ps_ver;
    }
    Viewport &v = st.vp;
    if (p->posFixup >= 0) glUniform4f(p->posFixup, v.w ? 1.0f / v.w : 0, v.h ? 1.0f / v.h : 0, 0, 0);
    uint32_t *rs = st.rs;
    if (p->alphaFunc >= 0) glUniform1i(p->alphaFunc, rs[15] ? (int)rs[25] : 8);
    if (p->alphaRef >= 0) glUniform1f(p->alphaRef, (rs[24] & 255) / 255.0f);
    if (p->vfaceSign >= 0) glUniform1f(p->vfaceSign, 1.0f);
    if (p->fogColor >= 0) { uint32_t fc = rs[34]; glUniform4f(p->fogColor, ((fc >> 16) & 255) / 255.f, ((fc >> 8) & 255) / 255.f, (fc & 255) / 255.f, 1.0f); }
    if (p->fogParams >= 0) {
        float mode = 0;
        bool sm3ps = p->ps && p->ps->sm.major >= 3;
        if (rs[28] && !sm3ps) {
            if (rs[35] == 0) mode = rs[140] || !p->ff_vs ? 1.0f : 0.0f;   // vertex fog (factor from VS)
            else mode = rs[35] == 3 ? 2.0f : rs[35] == 1 ? 3.0f : 4.0f;  // table fog LINEAR/EXP/EXP2
        }
        glUniform4f(p->fogParams, mode, fl(rs[36]), fl(rs[37]), fl(rs[38]));
    }
    if (p->ff_vs || p->ff_ps) {
        GLuint g = p->prog;
        auto U = [&](const char *n) { return glGetUniformLocation(g, n); };
        if (p->ff_vs) {
            glUniform4f(U("ffViewport"), (float)v.x, (float)v.y, (float)v.w, (float)v.h);
            glUniformMatrix4fv(U("ffWorld"), 1, GL_FALSE, st.transform[256]);
            glUniformMatrix4fv(U("ffView"), 1, GL_FALSE, st.transform[2]);
            glUniformMatrix4fv(U("ffProj"), 1, GL_FALSE, st.transform[3]);
            Material &m = st.mat;
            glUniform4fv(U("ffMatDiffuse"), 1, m.diffuse); glUniform4fv(U("ffMatAmbient"), 1, m.ambient);
            glUniform4fv(U("ffMatSpecular"), 1, m.specular); glUniform4fv(U("ffMatEmissive"), 1, m.emissive);
            glUniform1f(U("ffMatPower"), m.power);
            uint32_t a = rs[139];
            glUniform4f(U("ffAmbient"), ((a >> 16) & 255) / 255.f, ((a >> 8) & 255) / 255.f, (a & 255) / 255.f, (a >> 24) / 255.f);
            glUniform4f(U("ffFog"), fl(rs[36]), fl(rs[37]), fl(rs[38]), 0);
            float pos[8][4], dir[8][4], dif[8][4], spe[8][4], amb[8][4], att[8][4], spot[8][4];
            int n = 0;
            const float *V = st.transform[2];
            for (int i = 0; i < 8 && n < 8; i++) {
                if (!st.light_on[i]) continue;
                Light &L = st.lights[i];
                // transform into view space (row-vector convention: p' = p * V)
                pos[n][0] = L.pos[0] * V[0] + L.pos[1] * V[4] + L.pos[2] * V[8] + V[12];
                pos[n][1] = L.pos[0] * V[1] + L.pos[1] * V[5] + L.pos[2] * V[9] + V[13];
                pos[n][2] = L.pos[0] * V[2] + L.pos[1] * V[6] + L.pos[2] * V[10] + V[14];
                pos[n][3] = 1;
                dir[n][0] = L.dir[0] * V[0] + L.dir[1] * V[4] + L.dir[2] * V[8];
                dir[n][1] = L.dir[0] * V[1] + L.dir[1] * V[5] + L.dir[2] * V[9];
                dir[n][2] = L.dir[0] * V[2] + L.dir[1] * V[6] + L.dir[2] * V[10];
                float dl = sqrtf(dir[n][0] * dir[n][0] + dir[n][1] * dir[n][1] + dir[n][2] * dir[n][2]);
                if (dl > 0) for (int k = 0; k < 3; k++) dir[n][k] /= dl;
                dir[n][3] = 0;
                memcpy(dif[n], L.diffuse, 16); memcpy(spe[n], L.specular, 16); memcpy(amb[n], L.ambient, 16);
                att[n][0] = L.att0; att[n][1] = L.att1; att[n][2] = L.att2; att[n][3] = L.range > 0 ? L.range : 1e30f;
                spot[n][0] = cosf(L.theta / 2); spot[n][1] = cosf(L.phi / 2); spot[n][2] = L.falloff; spot[n][3] = (float)L.type;
                n++;
            }
            glUniform1i(U("ffNumLights"), n);
            if (n) {
                glUniform4fv(U("ffLightPos"), n, &pos[0][0]); glUniform4fv(U("ffLightDir"), n, &dir[0][0]);
                glUniform4fv(U("ffLightDiff"), n, &dif[0][0]); glUniform4fv(U("ffLightSpec"), n, &spe[0][0]);
                glUniform4fv(U("ffLightAmb"), n, &amb[0][0]); glUniform4fv(U("ffLightAtt"), n, &att[0][0]);
                glUniform4fv(U("ffLightSpot"), n, &spot[0][0]);
            }
        }
        if (p->ff_ps) {
            uint32_t tf = rs[60];
            glUniform4f(U("ffTFactor"), ((tf >> 16) & 255) / 255.f, ((tf >> 8) & 255) / 255.f, (tf & 255) / 255.f, (tf >> 24) / 255.f);
            float cs[8][4];
            for (int i = 0; i < 8; i++) { uint32_t k = st.tss[i][32]; cs[i][0] = ((k >> 16) & 255) / 255.f; cs[i][1] = ((k >> 8) & 255) / 255.f; cs[i][2] = (k & 255) / 255.f; cs[i][3] = (k >> 24) / 255.f; }
            glUniform4fv(U("ffConst"), 8, &cs[0][0]);
        }
    }
}

// ====================================================================== vertex input setup
struct AttrFmt { GLint size; GLenum type; GLboolean norm; };
static AttrFmt attr_fmt(uint8_t t) {
    switch (t) {
    case 0: return {1, GL_FLOAT, GL_FALSE};
    case 1: return {2, GL_FLOAT, GL_FALSE};
    case 2: return {3, GL_FLOAT, GL_FALSE};
    case 3: return {4, GL_FLOAT, GL_FALSE};
    case 4: return {GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE};
    case 5: return {4, GL_UNSIGNED_BYTE, GL_FALSE};
    case 6: return {2, GL_SHORT, GL_FALSE};
    case 7: return {4, GL_SHORT, GL_FALSE};
    case 8: return {4, GL_UNSIGNED_BYTE, GL_TRUE};
    case 9: return {2, GL_SHORT, GL_TRUE};
    case 10: return {4, GL_SHORT, GL_TRUE};
    case 11: return {2, GL_UNSIGNED_SHORT, GL_TRUE};
    case 12: return {4, GL_UNSIGNED_SHORT, GL_TRUE};
    case 13: return {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_FALSE};
    case 14: return {4, GL_INT_2_10_10_10_REV, GL_TRUE};
    case 15: return {2, GL_HALF_FLOAT, GL_FALSE};
    case 16: return {4, GL_HALF_FLOAT, GL_FALSE};
    default: return {0, 0, GL_FALSE};
    }
}

static std::map<uint32_t, Decl *> g_fvf_decls;
static Decl *decl_from_fvf(uint32_t fvf) {
    auto it = g_fvf_decls.find(fvf);
    if (it != g_fvf_decls.end()) return it->second;
    Decl *d = new Decl();
    d->fvf = fvf;
    uint16_t off = 0;
    uint32_t pos = fvf & 0x400E;
    auto add = [&](uint8_t type, uint8_t usage, uint8_t idx, uint16_t sz) { d->el.push_back({0, off, type, 0, usage, idx}); off += sz; };
    if (pos == 0x4) { add(3, 9, 0, 16); d->pretransformed = true; }
    else if (pos == 0x4002) add(3, 0, 0, 16);
    else if (pos) {
        add(2, 0, 0, 12);
        int nb = pos >= 6 ? (int)((pos - 4) / 2) : 0;    // XYZB1..5
        if (nb) {
            bool lastub = fvf & 0x1000 || fvf & 0x8000;
            int nw = lastub ? nb - 1 : nb;
            if (nw > 0) add((uint8_t)(nw - 1), 1, 0, (uint16_t)(4 * nw));
            if (lastub) add(5, 2, 0, 4);
        }
    }
    if (fvf & 0x10) add(2, 3, 0, 12);
    if (fvf & 0x20) add(0, 4, 0, 4);
    if (fvf & 0x40) add(4, 10, 0, 4);
    if (fvf & 0x80) add(4, 10, 1, 4);
    int ntex = (fvf >> 8) & 0xF;
    for (int i = 0; i < ntex; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        int n = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        add((uint8_t)(n - 1), 5, (uint8_t)i, (uint16_t)(4 * n));
    }
    g_fvf_decls[fvf] = d;
    return d;
}

static Decl *current_decl() {
    if (D->st.decl) return D->st.decl;
    if (D->st.fvf) return decl_from_fvf(D->st.fvf);
    return nullptr;
}

// bind vertex attributes; returns number of instances (1 if no instancing)
static int setup_attribs(Decl *decl, Program *p, GLuint up_buf, uint32_t up_stride, uint32_t base_vertex) {
    uint32_t enabled = 0;
    int instances = 1;
    if ((D->st.streams[0].freq >> 30) == 1) instances = std::max<int>(1, D->st.streams[0].freq & 0x3FFFFFFF);
    if (!decl) goto done;
    for (auto &e : decl->el) {
        int loc = -1;
        if (p->vs) {
            for (auto &in : p->vs->sm.inputs)
                if (in.usage == e.usage && in.index == e.index) { loc = atoi(in.reg.c_str() + 1); break; }
                else if (e.usage == 9 && in.usage == 0 && in.index == e.index) { loc = atoi(in.reg.c_str() + 1); break; }
        } else loc = ff_attrib_location(e.usage, e.index);
        if (loc < 0 || loc >= 16) continue;
        AttrFmt af = attr_fmt(e.type);
        if (!af.size) continue;
        StreamSrc &s = D->st.streams[e.stream];
        GLuint buf; uint32_t stride, off;
        if (up_buf && e.stream == 0) { buf = up_buf; stride = up_stride; off = e.offset; }
        else {
            if (!s.vb) continue;
            buf = s.vb->buf; stride = s.stride; off = s.offset + e.offset + base_vertex * s.stride;
        }
        glBindBuffer(GL_ARRAY_BUFFER, buf);
        glVertexAttribPointer(loc, af.size, af.type, af.norm, stride, (const void *)(uintptr_t)off);
        bool inst = instances > 1 && e.stream > 0 && (s.freq >> 30) == 2;
        glVertexAttribDivisor(loc, inst ? (s.freq & 0x3FFFFFFF) : 0);
        enabled |= 1u << loc;
    }
done:
    uint32_t diff = enabled ^ D->enabled_attribs;
    for (int i = 0; i < 16; i++) if (diff & (1u << i)) {
        if (enabled & (1u << i)) glEnableVertexAttribArray(i);
        else { glDisableVertexAttribArray(i); glVertexAttrib4f(i, 0, 0, 0, 1); }
    }
    D->enabled_attribs = enabled;
    return instances;
}

static void bind_textures(Program *p) {
    for (int i = 0; i < 20; i++) {
        bool used = i < 16 ? (p->ps ? (p->ps->sm.samplers_used >> i) & 1 : i < 8) : (p->vs ? (p->vs->sm.samplers_used >> (i - 16)) & 1 : 0);
        if (!used) continue;
        if (D->samp_dirty[i]) apply_sampler(i);
        Texture *t = static_cast<Texture *>(D->st.tex[i]);
        glActiveTexture(GL_TEXTURE0 + i);
        GLenum want = GL_TEXTURE_2D;
        if (p->ps && i < 16) want = p->ps->sm.sampler_type[i] == 3 ? GL_TEXTURE_CUBE_MAP : p->ps->sm.sampler_type[i] == 4 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
        if (p->vs && i >= 16) want = p->vs->sm.sampler_type[i - 16] == 3 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
        if (t && t->tex) {
            if (!p->ps && i < 16) want = t->target;
            glBindTexture(t->target, t->tex);
            if (t->target != want) glBindTexture(want, 0);
        } else glBindTexture(want, 0);
        glBindSampler(i, D->samplers[i]);
    }
    glActiveTexture(GL_TEXTURE0);
}

static GLenum prim_mode(uint32_t t, uint32_t n, uint32_t *count) {
    switch (t) {
    case 1: *count = n; return GL_POINTS;
    case 2: *count = n * 2; return GL_LINES;
    case 3: *count = n + 1; return GL_LINE_STRIP;
    case 4: *count = n * 3; return GL_TRIANGLES;
    case 5: *count = n + 2; return GL_TRIANGLE_STRIP;
    case 6: *count = n + 2; return GL_TRIANGLE_FAN;
    default: *count = 0; return GL_TRIANGLES;
    }
}

static Program *prepare_draw() {
    bind_rt_fbo();
    if (D->vp_dirty) apply_viewport();
    if (D->rs_dirty) apply_render_state();
    Decl *decl = current_decl();
    Program *p = get_program(decl);
    glUseProgram(p->prog);
    upload_consts(p);
    bind_textures(p);
    if (D->rt[0] && D->rt[0]->container) D->rt[0]->container->rendered = true;   // mips need regeneration
    return p;
}

// ====================================================================== device: setters used by state blocks
static void set_texture(int stage, Obj *t) {
    Obj *&cur = D->st.tex[stage];
    if (cur == t) return;
    if (t) obj_addref(t);
    Obj *old = cur;
    cur = t;
    if (old) obj_release(old);
}
static void set_stream(int i, Buffer *vb, uint32_t off, uint32_t stride) {
    StreamSrc &s = D->st.streams[i];
    if (s.vb != vb) { if (vb) obj_addref(vb); Buffer *o = s.vb; s.vb = vb; if (o) obj_release(o); }
    s.offset = off; s.stride = stride;
}
static void set_indices(Buffer *ib) {
    if (D->st.ib == ib) return;
    if (ib) obj_addref(ib);
    Buffer *o = D->st.ib; D->st.ib = ib;
    if (o) obj_release(o);
}
static void set_shader(bool ps, Shader *s) {
    Shader *&cur = ps ? D->st.ps : D->st.vs;
    if (cur == s) return;
    if (s) obj_addref(s);
    Shader *o = cur; cur = s;
    if (o) obj_release(o);
}
static void set_decl(Decl *d) {
    if (D->st.decl == d) return;
    if (d) obj_addref(d);
    Decl *o = D->st.decl; D->st.decl = d;
    if (o) obj_release(o);
}
static void set_rt(int i, Surface *s) {
    if (D->rt[i] == s) return;
    if (s) obj_addref(s);
    Surface *o = D->rt[i]; D->rt[i] = s;
    if (o) obj_release(o);
    D->fbo_dirty = true;
    D->rs_dirty = true;
}
static void set_ds(Surface *s) {
    if (D->ds == s) return;
    if (s) obj_addref(s);
    Surface *o = D->ds; D->ds = s;
    if (o) obj_release(o);
    D->fbo_dirty = true;
    D->rs_dirty = true;
}

static void reset_state() {
    DevState &st = D->st;
    memset(st.rs, 0, sizeof st.rs);
    uint32_t *rs = st.rs;
    auto F = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
    rs[7] = D->pp[9] ? 1 : 0; rs[8] = 3; rs[9] = 2; rs[14] = 1; rs[16] = 1; rs[19] = 2; rs[20] = 1; rs[22] = 3; rs[23] = 4;
    rs[25] = 8; rs[34] = 0; rs[36] = 0; rs[37] = F(1.0f); rs[38] = F(1.0f);
    rs[53] = rs[54] = rs[55] = 1; rs[56] = 8; rs[58] = 0xFFFFFFFF; rs[59] = 0xFFFFFFFF; rs[60] = 0xFFFFFFFF;
    rs[136] = 1; rs[137] = 1; rs[141] = 1; rs[142] = 1; rs[145] = 1; rs[146] = 2;
    rs[154] = F(1.0f); rs[155] = F(1.0f); rs[158] = F(1.0f); rs[161] = 1; rs[162] = 0xFFFFFFFF; rs[166] = F(64.0f);
    rs[168] = 0xF; rs[171] = 1; rs[186] = rs[187] = rs[188] = 1; rs[189] = 8;
    rs[190] = rs[191] = rs[192] = 0xF; rs[193] = 0xFFFFFFFF; rs[207] = 2; rs[208] = 1; rs[209] = 1;
    memset(st.tss, 0, sizeof st.tss);
    for (int i = 0; i < 8; i++) {
        st.tss[i][1] = i == 0 ? 4 : 1; st.tss[i][2] = 2; st.tss[i][3] = 1;
        st.tss[i][4] = i == 0 ? 2 : 1; st.tss[i][5] = 2; st.tss[i][6] = 1;
        st.tss[i][11] = i; st.tss[i][26] = 1; st.tss[i][27] = 1; st.tss[i][28] = 1;
    }
    for (int i = 0; i < 20; i++) {
        memset(st.samp[i], 0, sizeof st.samp[i]);
        st.samp[i][1] = st.samp[i][2] = st.samp[i][3] = 1;
        st.samp[i][5] = st.samp[i][6] = 1; st.samp[i][7] = 0; st.samp[i][10] = 1;
        D->samp_dirty[i] = true;
    }
    memset(st.vsf, 0, sizeof st.vsf); memset(st.psf, 0, sizeof st.psf);
    memset(st.vsi, 0, sizeof st.vsi); memset(st.psi, 0, sizeof st.psi);
    memset(st.vsb, 0, sizeof st.vsb); memset(st.psb, 0, sizeof st.psb);
    for (int i = 0; i < 512; i++) { memset(st.transform[i], 0, 64); for (int k = 0; k < 4; k++) st.transform[i][k * 5] = 1; }
    memset(st.lights, 0, sizeof st.lights); memset(st.light_on, 0, sizeof st.light_on);
    memset(&st.mat, 0, sizeof st.mat);
    memset(st.clip, 0, sizeof st.clip);
    for (int i = 0; i < 20; i++) set_texture(i, nullptr);
    for (int i = 0; i < 16; i++) { set_stream(i, nullptr, 0, 0); st.streams[i].freq = 1; }
    set_indices(nullptr); set_decl(nullptr); st.fvf = 0;
    set_shader(false, nullptr); set_shader(true, nullptr);
    D->vs_ver++; D->ps_ver++;
    D->rs_dirty = true;
}

// ====================================================================== back buffer / presentation
static void create_backbuffer() {
    uint32_t w = D->pp[0], h = D->pp[1];
    uint32_t fmt = D->pp[2] ? D->pp[2] : 22;
    if (fmt == 22 || fmt == 23 || fmt == 24) fmt = 22;   // render as X8R8G8B8
    Surface *bb = create_surface(w, h, fmt == 21 ? 21 : 22, 1, 0, 0);
    obj_publish(bb, g_vt[K_SURF]);
    D->backbuf = bb;
    set_rt(0, bb);
    obj_release(bb);   // owned by device binding + backbuf pointer (extra ref below)
    obj_addref(bb);
    for (int i = 1; i < 4; i++) set_rt(i, nullptr);
    if (D->pp[9]) {
        Surface *ds = create_surface(w, h, D->pp[10] ? D->pp[10] : 75, 2, 0, 0);
        obj_publish(ds, g_vt[K_SURF]);
        D->autods = ds;
        set_ds(ds);
        obj_release(ds);
        obj_addref(ds);
    } else set_ds(nullptr);
    D->st.vp = {0, 0, w, h, 0.0f, 1.0f};
    D->st.scissor[0] = 0; D->st.scissor[1] = 0; D->st.scissor[2] = (int)w; D->st.scissor[3] = (int)h;
    D->vp_dirty = true;
}

static void apply_window_mode() {
    bool windowed = D->pp[8] != 0;
    uint32_t w = D->pp[0], h = D->pp[1];
    if (windowed) {
        SDL_SetWindowFullscreen(g_win, false);
        SDL_SetWindowSize(g_win, (int)w, (int)h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        SDL_SetWindowFullscreenMode(g_win, nullptr);      // borderless desktop fullscreen, back buffer is scaled
        SDL_SetWindowFullscreen(g_win, true);
    }
    SDL_ShowWindow(g_win);
    SDL_SyncWindow(g_win);
    uint32_t iv = D->pp[13];
    SDL_GL_SetSwapInterval(iv == 0x80000000u ? 0 : 1);
}

static void debug_shot() {
    static const char *frames = getenv("FVR_SHOT_FRAMES");
    static const char *dir = getenv("FVR_SHOT_DIR");
    if (!frames || !dir) return;
    char key[32]; snprintf(key, sizeof key, ",%llu,", (unsigned long long)D->frame);
    std::string list = std::string(",") + frames + ",";
    if (list.find(key) == std::string::npos) return;
    uint32_t w = D->backbuf->w, h = D->backbuf->h;
    std::vector<uint8_t> px((size_t)w * h * 4);
    GLuint f = fbo_for(&D->backbuf, 1, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%05llu.ppm", dir, (unsigned long long)D->frame);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) fwrite(&px[i * 4], 1, 3, o);
    fclose(o);
    rtlog("d3d9: saved %s", path);
}

static void present() {
    if (!D->backbuf) return;
    debug_shot();
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    GLuint src = fbo_for(&D->backbuf, 1, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    uint32_t bw = D->backbuf->w, bh = D->backbuf->h;
    // letterbox, keep aspect
    double sx = (double)ww / bw, sy = (double)wh / bh, s = std::min(sx, sy);
    int dw = (int)(bw * s + 0.5), dh = (int)(bh * s + 0.5);
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    glBlitFramebuffer(0, 0, bw, bh, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(g_win);
    D->fbo_dirty = true;
    D->rs_dirty = true;
    D->vp_dirty = true;
    D->frame++;
    if (trace() && D->frame % 300 == 0) rtlog("d3d9: frame %llu", (unsigned long long)D->frame);
}

// ====================================================================== IDirect3DDevice9
M(dev_TestCooperativeLevel) { RETN(1, D3D_OK); }
M(dev_GetAvailableTextureMem) { RETN(1, 0x3FF00000); }
M(dev_EvictManagedResources) { RETN(1, D3D_OK); }
M(dev_GetDirect3D) { GUARD; W32(ARG(1), D->d3d); obj_addref(obj_get(D->d3d)); RETN(2, D3D_OK); }
M(dev_GetDeviceCaps) { fill_caps(ARG(1)); RETN(2, D3D_OK); }
static void write_display_mode(uint32_t p) {
    ui_ensure_sdl();
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    W32(p, dm ? dm->w : 1920); W32(p + 4, dm ? dm->h : 1080);
    W32(p + 8, dm && dm->refresh_rate > 0 ? (uint32_t)(dm->refresh_rate + 0.5f) : 60); W32(p + 12, 22);
}
M(dev_GetDisplayMode) {
    GUARD;
    if (!D->pp[8]) { uint32_t p = ARG(2); W32(p, D->pp[0]); W32(p + 4, D->pp[1]); W32(p + 8, 60); W32(p + 12, 22); }
    else write_display_mode(ARG(2));
    RETN(3, D3D_OK);
}
M(dev_GetCreationParameters) { GUARD; uint32_t p = ARG(1); W32(p, 0); W32(p + 4, 1); W32(p + 8, D->hwnd); W32(p + 12, D->behavior); RETN(2, D3D_OK); }
M(dev_SetCursorProperties) { RETN(4, D3D_OK); }
M(dev_SetCursorPosition) { RETN(4, 0); }
M(dev_ShowCursor) { GUARD; int prev = D->cursor_shown; D->cursor_shown = ARG(1) ? 1 : 0; if (D->cursor_shown) SDL_ShowCursor(); else SDL_HideCursor(); RETN(2, prev); }
M(dev_CreateAdditionalSwapChain) { rtlog("d3d9: CreateAdditionalSwapChain not supported"); RETN(3, D3DERR_NOTAVAILABLE); }
M(dev_GetSwapChain) { GUARD; W32(ARG(2), D->swap); obj_addref(obj_get(D->swap)); RETN(3, D3D_OK); }
M(dev_GetNumberOfSwapChains) { RETN(1, 1); }
M(dev_Reset) {
    GUARD;
    for (int i = 0; i < 14; i++) D->pp[i] = R32(ARG(1) + 4 * i);
    if (!D->pp[0] || !D->pp[1]) { int w, h; SDL_GetWindowSize(g_win, &w, &h); D->pp[0] = w; D->pp[1] = h; W32(ARG(1), w); W32(ARG(1) + 4, h); }
    rtlog("d3d9: Reset %ux%u windowed=%u", D->pp[0], D->pp[1], D->pp[8]);
    reset_state();
    set_rt(0, nullptr); set_ds(nullptr);
    if (D->backbuf) { Surface *b = D->backbuf; D->backbuf = nullptr; obj_release(b); }
    if (D->autods) { Surface *b = D->autods; D->autods = nullptr; obj_release(b); }
    create_backbuffer();
    apply_window_mode();
    RETN(2, D3D_OK);
}
M(dev_Present) { GUARD; present(); RETN(5, D3D_OK); }
M(dev_GetBackBuffer) { GUARD; W32(ARG(4), D->backbuf->guest); obj_addref(D->backbuf); RETN(5, D3D_OK); }
M(dev_GetRasterStatus) { W32(ARG(2), 0); W32(ARG(2) + 4, 0); RETN(3, D3D_OK); }
M(dev_SetDialogBoxMode) { RETN(2, D3D_OK); }
M(dev_SetGammaRamp) { GUARD; memcpy(D->gamma, H(ARG(3)), sizeof D->gamma); RETN(4, 0); }
M(dev_GetGammaRamp) {
    GUARD;
    if (!D->gamma[0][255]) for (int c2 = 0; c2 < 3; c2++) for (int i = 0; i < 256; i++) D->gamma[c2][i] = (uint16_t)(i * 257);
    memcpy(H(ARG(2)), D->gamma, sizeof D->gamma);
    RETN(3, 0);
}
M(dev_CreateTexture) {
    GUARD;
    Texture *t = create_texture(K_TEX, ARG(1), ARG(2), 1, ARG(3), ARG(4), ARG(5), ARG(6));
    if (!t) RETN(9, D3DERR_INVALIDCALL);
    W32(ARG(7), obj_publish(t, g_vt[K_TEX]));
    if (trace()) rtlog("d3d9: CreateTexture %ux%u lv%u usage %x fmt %x pool %u -> %08x", ARG(1), ARG(2), t->levels, ARG(4), ARG(5), ARG(6), t->guest);
    RETN(9, D3D_OK);
}
M(dev_CreateVolumeTexture) {
    GUARD;
    Texture *t = create_texture(K_VOL, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7));
    if (!t) RETN(10, D3DERR_INVALIDCALL);
    W32(ARG(8), obj_publish(t, g_vt[K_VOL]));
    RETN(10, D3D_OK);
}
M(dev_CreateCubeTexture) {
    GUARD;
    Texture *t = create_texture(K_CUBE, ARG(1), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5));
    if (!t) RETN(8, D3DERR_INVALIDCALL);
    W32(ARG(6), obj_publish(t, g_vt[K_CUBE]));
    RETN(8, D3D_OK);
}
M(dev_CreateVertexBuffer) {
    GUARD;
    Buffer *b = create_buffer(K_VB, ARG(1), ARG(2), ARG(3), ARG(4));
    W32(ARG(5), obj_publish(b, g_vt[K_VB]));
    RETN(7, D3D_OK);
}
M(dev_CreateIndexBuffer) {
    GUARD;
    Buffer *b = create_buffer(K_IB, ARG(1), ARG(2), ARG(3), ARG(4));
    W32(ARG(5), obj_publish(b, g_vt[K_IB]));
    RETN(7, D3D_OK);
}
M(dev_CreateRenderTarget) {
    GUARD;
    Surface *s = create_surface(ARG(1), ARG(2), ARG(3), 1, 0, 0);
    if (!s) RETN(9, D3DERR_INVALIDCALL);
    W32(ARG(7), obj_publish(s, g_vt[K_SURF]));
    RETN(9, D3D_OK);
}
M(dev_CreateDepthStencilSurface) {
    GUARD;
    Surface *s = create_surface(ARG(1), ARG(2), ARG(3), 2, 0, 0);
    if (!s) RETN(9, D3DERR_INVALIDCALL);
    W32(ARG(7), obj_publish(s, g_vt[K_SURF]));
    RETN(9, D3D_OK);
}
M(dev_CreateOffscreenPlainSurface) {
    GUARD;
    Surface *s = create_surface(ARG(1), ARG(2), ARG(3), 0, ARG(4) == 0 ? 2 : ARG(4), 0);
    if (!s) RETN(7, D3DERR_INVALIDCALL);
    W32(ARG(5), obj_publish(s, g_vt[K_SURF]));
    RETN(7, D3D_OK);
}

// copy CPU data of a sysmem surface/texture level into a GL-backed surface region
static void upload_surface_rect(Surface *dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h, const uint8_t *src, uint32_t pitch) {
    const FmtInfo *fi = fmt_info(dst->fmt);
    if (!fi || fi->depth) return;
    GLuint tex = dst->gl_tex();
    if (!tex) return;
    GLenum tgt = dst->gl_target();
    glBindTexture(dst->container && dst->container->kind == K_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (fi->compressed) {
        glCompressedTexSubImage2D(tgt, dst->level, dx, dy, w, h, fi->internal, pitch * fmt_rows(fi, h), src);
    } else {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / fi->bpp);
        glTexSubImage2D(tgt, dst->level, dx, dy, w, h, fi->format, fi->type, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}
static uint32_t surface_cpu(Surface *s, uint32_t *pitch) {
    const FmtInfo *fi = fmt_info(s->fmt);
    *pitch = fmt_pitch(fi, s->w);
    if (s->container) return s->container->mem[s->face * s->container->levels + s->level];
    return s->mem;
}

M(dev_UpdateSurface) {
    GUARD;
    Surface *src = OBJ<Surface>(ARG(1)), *dst = OBJ<Surface>(ARG(3));
    uint32_t pitch, m = surface_cpu(src, &pitch);
    if (!m) RETN(5, D3D_OK);
    uint32_t sx = 0, sy = 0, w = src->w, h = src->h;
    if (ARG(2)) { sx = R32(ARG(2)); sy = R32(ARG(2) + 4); w = R32(ARG(2) + 8) - sx; h = R32(ARG(2) + 12) - sy; }
    uint32_t dx = ARG(4) ? R32(ARG(4)) : 0, dy = ARG(4) ? R32(ARG(4) + 4) : 0;
    const FmtInfo *fi = fmt_info(src->fmt);
    uint32_t off = fi->compressed ? (sy / 4) * pitch + (sx / 4) * fi->bpp : sy * pitch + sx * fi->bpp;
    upload_surface_rect(dst, dx, dy, w, h, H(m + off), pitch);
    RETN(5, D3D_OK);
}
M(dev_UpdateTexture) {
    GUARD;
    Texture *src = OBJ<Texture>(ARG(1)), *dst = OBJ<Texture>(ARG(2));
    if (!src || !dst) RETN(3, D3DERR_INVALIDCALL);
    int faces = src->kind == K_CUBE ? 6 : 1;
    uint32_t skip = src->levels > dst->levels ? src->levels - dst->levels : 0;
    for (int f = 0; f < faces; f++)
        for (uint32_t l = 0; l + skip < src->levels && l < dst->levels; l++) {
            uint32_t m = src->mem[f * src->levels + l + skip];
            if (!m) continue;
            uint32_t &dm = dst->mem[f * dst->levels + l];
            uint32_t sz = level_size(dst, l, nullptr, nullptr);
            bool tmp = !dm;
            if (tmp) dm = m;
            upload_level(dst, f, l);
            if (tmp) dm = 0;
            (void)sz;
        }
    RETN(3, D3D_OK);
}
static void read_surface_into(Surface *src, uint32_t dst_mem, uint32_t dst_fmt, uint32_t w, uint32_t h) {
    const FmtInfo *fi = fmt_info(dst_fmt);
    if (!fi || fi->compressed || fi->depth) return;
    GLuint f = fbo_for(&src, 1, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, fi->format, fi->type, H(dst_mem));
    glBindFramebuffer(GL_FRAMEBUFFER, D->cur_fbo);
    D->fbo_dirty = true;
}
M(dev_GetRenderTargetData) {
    GUARD;
    Surface *src = OBJ<Surface>(ARG(1)), *dst = OBJ<Surface>(ARG(2));
    uint32_t pitch, m = surface_cpu(dst, &pitch);
    if (!m && !dst->container) { dst->mem = gmalloc(pitch * dst->h + 64, true); m = dst->mem; }
    if (m) read_surface_into(src, m, dst->fmt, std::min(src->w, dst->w), std::min(src->h, dst->h));
    RETN(3, D3D_OK);
}
M(dev_GetFrontBufferData) {
    GUARD;
    Surface *dst = OBJ<Surface>(ARG(2));
    uint32_t pitch, m = surface_cpu(dst, &pitch);
    if (m) read_surface_into(D->backbuf, m, dst->fmt, std::min(D->backbuf->w, dst->w), std::min(D->backbuf->h, dst->h));
    RETN(3, D3D_OK);
}
M(dev_StretchRect) {
    GUARD;
    Surface *src = OBJ<Surface>(ARG(1)), *dst = OBJ<Surface>(ARG(3));
    if (!src || !dst) RETN(6, D3DERR_INVALIDCALL);
    int sx0 = 0, sy0 = 0, sx1 = src->w, sy1 = src->h, dx0 = 0, dy0 = 0, dx1 = dst->w, dy1 = dst->h;
    if (ARG(2)) { sx0 = R32(ARG(2)); sy0 = R32(ARG(2) + 4); sx1 = R32(ARG(2) + 8); sy1 = R32(ARG(2) + 12); }
    if (ARG(4)) { dx0 = R32(ARG(4)); dy0 = R32(ARG(4) + 4); dx1 = R32(ARG(4) + 8); dy1 = R32(ARG(4) + 12); }
    const FmtInfo *fs = fmt_info(src->fmt);
    bool depth = fs && fs->depth;
    if (!src->gl_tex() && src->mem) {   // sysmem -> GL surface
        uint32_t pitch; uint32_t m = surface_cpu(src, &pitch);
        upload_surface_rect(dst, dx0, dy0, dx1 - dx0, dy1 - dy0, H(m + sy0 * pitch + sx0 * fs->bpp), pitch);
        RETN(6, D3D_OK);
    }
    GLuint rf = depth ? fbo_for(nullptr, 0, src) : fbo_for(&src, 1, nullptr);
    GLuint df = depth ? fbo_for(nullptr, 0, dst) : fbo_for(&dst, 1, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
                      depth ? (GL_DEPTH_BUFFER_BIT | (fs->stencil ? GL_STENCIL_BUFFER_BIT : 0)) : GL_COLOR_BUFFER_BIT,
                      depth || ARG(5) == 1 ? GL_NEAREST : GL_LINEAR);
    D->fbo_dirty = true; D->rs_dirty = true;
    if (dst->container && dst->container->autogen && dst->level == 0) { glBindTexture(dst->container->target, dst->container->tex); glGenerateMipmap(dst->container->target); }
    RETN(6, D3D_OK);
}
M(dev_ColorFill) {
    GUARD;
    Surface *s = OBJ<Surface>(ARG(1));
    uint32_t col = ARG(3);
    GLuint f = fbo_for(&s, 1, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glColorMask(1, 1, 1, 1);
    if (ARG(2)) { glEnable(GL_SCISSOR_TEST); int l = R32(ARG(2)), t = R32(ARG(2) + 4); glScissor(l, t, R32(ARG(2) + 8) - l, R32(ARG(2) + 12) - t); }
    else glDisable(GL_SCISSOR_TEST);
    glClearColor(((col >> 16) & 255) / 255.f, ((col >> 8) & 255) / 255.f, (col & 255) / 255.f, (col >> 24) / 255.f);
    glClear(GL_COLOR_BUFFER_BIT);
    D->fbo_dirty = true; D->rs_dirty = true;
    RETN(4, D3D_OK);
}
M(dev_SetRenderTarget) {
    GUARD;
    uint32_t i = ARG(1);
    if (i >= 4) RETN(3, D3DERR_INVALIDCALL);
    Surface *s = OBJ<Surface>(ARG(2));
    if (i == 0 && !s) RETN(3, D3DERR_INVALIDCALL);
    set_rt((int)i, s);
    if (i == 0) {
        D->st.vp = {0, 0, s->w, s->h, 0.0f, 1.0f};
        D->st.scissor[0] = 0; D->st.scissor[1] = 0; D->st.scissor[2] = (int)s->w; D->st.scissor[3] = (int)s->h;
        D->vp_dirty = true;
    }
    RETN(3, D3D_OK);
}
M(dev_GetRenderTarget) {
    GUARD;
    Surface *s = ARG(1) < 4 ? D->rt[ARG(1)] : nullptr;
    if (!s) { W32(ARG(2), 0); RETN(3, D3DERR_NOTFOUND); }
    obj_addref(s); W32(ARG(2), s->guest);
    RETN(3, D3D_OK);
}
M(dev_SetDepthStencilSurface) { GUARD; set_ds(OBJ<Surface>(ARG(1))); RETN(2, D3D_OK); }
M(dev_GetDepthStencilSurface) {
    GUARD;
    if (!D->ds) { W32(ARG(1), 0); RETN(2, D3DERR_NOTFOUND); }
    obj_addref(D->ds); W32(ARG(1), D->ds->guest);
    RETN(2, D3D_OK);
}
M(dev_BeginScene) { GUARD; D->in_scene = true; RETN(1, D3D_OK); }
M(dev_EndScene) { GUARD; D->in_scene = false; glFlush(); RETN(1, D3D_OK); }
M(dev_Clear) {
    GUARD;
    uint32_t count = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4), stencil = ARG(6);
    float z = ARGF(5);
    bind_rt_fbo();
    GLbitfield mask = 0;
    if (flags & 1) { mask |= GL_COLOR_BUFFER_BIT; glColorMask(1, 1, 1, 1); for (int i = 1; i < 4; i++) glColorMaski(i, 1, 1, 1, 1);
        glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, (color >> 24) / 255.f); }
    if ((flags & 2) && D->ds) { mask |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); glClearDepth(z); }
    if ((flags & 4) && D->ds) { mask |= GL_STENCIL_BUFFER_BIT; glStencilMask(0xFFFFFFFF); glClearStencil((GLint)stencil); }
    if (mask) {
        Viewport &v = D->st.vp;
        int x0 = v.x, y0 = v.y, x1 = v.x + v.w, y1 = v.y + v.h;
        if (D->st.rs[174]) { x0 = std::max(x0, D->st.scissor[0]); y0 = std::max(y0, D->st.scissor[1]); x1 = std::min(x1, D->st.scissor[2]); y1 = std::min(y1, D->st.scissor[3]); }
        glEnable(GL_SCISSOR_TEST);
        if (count && rects) {
            for (uint32_t i = 0; i < count; i++) {
                uint32_t r = rects + 16 * i;
                int a = std::max(x0, (int)R32(r)), b = std::max(y0, (int)R32(r + 4)), cc = std::min(x1, (int)R32(r + 8)), d = std::min(y1, (int)R32(r + 12));
                if (cc <= a || d <= b) continue;
                glScissor(a, b, cc - a, d - b);
                glClear(mask);
            }
        } else if (x1 > x0 && y1 > y0) {
            glScissor(x0, y0, x1 - x0, y1 - y0);
            glClear(mask);
        }
    }
    D->rs_dirty = true;
    RETN(7, D3D_OK);
}
M(dev_SetTransform) { GUARD; uint32_t s = ARG(1); if (s < 512) memcpy(D->st.transform[s], H(ARG(2)), 64); RETN(3, D3D_OK); }
M(dev_GetTransform) { GUARD; uint32_t s = ARG(1); if (s < 512) memcpy(H(ARG(2)), D->st.transform[s], 64); RETN(3, D3D_OK); }
M(dev_MultiplyTransform) {
    GUARD; uint32_t s = ARG(1);
    if (s < 512) {
        float a[16], b[16], r[16];
        memcpy(a, D->st.transform[s], 64); memcpy(b, H(ARG(2)), 64);
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = v; }
        memcpy(D->st.transform[s], r, 64);
    }
    RETN(3, D3D_OK);
}
M(dev_SetViewport) {
    GUARD; uint32_t p = ARG(1);
    Viewport &v = D->st.vp;
    v.x = R32(p); v.y = R32(p + 4); v.w = R32(p + 8); v.h = R32(p + 12); v.minz = RF(p + 16); v.maxz = RF(p + 20);
    D->vp_dirty = true;
    RETN(2, D3D_OK);
}
M(dev_GetViewport) {
    GUARD; uint32_t p = ARG(1); Viewport &v = D->st.vp;
    W32(p, v.x); W32(p + 4, v.y); W32(p + 8, v.w); W32(p + 12, v.h); WF(p + 16, v.minz); WF(p + 20, v.maxz);
    RETN(2, D3D_OK);
}
M(dev_SetMaterial) { GUARD; memcpy(&D->st.mat, H(ARG(1)), 68); RETN(2, D3D_OK); }
M(dev_GetMaterial) { GUARD; memcpy(H(ARG(1)), &D->st.mat, 68); RETN(2, D3D_OK); }
M(dev_SetLight) {
    GUARD;
    uint32_t i = ARG(1), p = ARG(2);
    if (i < 8) {
        Light &L = D->st.lights[i];
        L.type = R32(p);
        memcpy(L.diffuse, H(p + 4), 16); memcpy(L.specular, H(p + 20), 16); memcpy(L.ambient, H(p + 36), 16);
        memcpy(L.pos, H(p + 52), 12); memcpy(L.dir, H(p + 64), 12);
        L.range = RF(p + 76); L.falloff = RF(p + 80); L.att0 = RF(p + 84); L.att1 = RF(p + 88); L.att2 = RF(p + 92);
        L.theta = RF(p + 96); L.phi = RF(p + 100);
    }
    RETN(3, D3D_OK);
}
M(dev_GetLight) { RETN(3, D3D_OK); }
M(dev_LightEnable) { GUARD; if (ARG(1) < 8) D->st.light_on[ARG(1)] = ARG(2) != 0; RETN(3, D3D_OK); }
M(dev_GetLightEnable) { GUARD; W32(ARG(2), ARG(1) < 8 && D->st.light_on[ARG(1)]); RETN(3, D3D_OK); }
M(dev_SetClipPlane) { GUARD; if (ARG(1) < 8) memcpy(D->st.clip[ARG(1)], H(ARG(2)), 16); RETN(3, D3D_OK); }
M(dev_GetClipPlane) { GUARD; if (ARG(1) < 8) memcpy(H(ARG(2)), D->st.clip[ARG(1)], 16); RETN(3, D3D_OK); }
M(dev_SetRenderState) {
    GUARD;
    uint32_t s = ARG(1), v = ARG(2);
    if (s < 256) {
        D->st.rs[s] = v;
        D->rs_dirty = true;
        if (D->recording) D->recording->rs[s] = true;
    }
    RETN(3, D3D_OK);
}
M(dev_GetRenderState) { GUARD; W32(ARG(2), ARG(1) < 256 ? D->st.rs[ARG(1)] : 0); RETN(3, D3D_OK); }
M(dev_CreateStateBlock) {
    GUARD;
    StateBlock *b = new StateBlock();
    sb_mark_all(b, ARG(1));
    sb_capture(b);
    W32(ARG(2), obj_publish(b, g_vt[K_SB]));
    RETN(3, D3D_OK);
}
M(dev_BeginStateBlock) {
    GUARD;
    D->recording = new StateBlock();
    RETN(1, D3D_OK);
}
M(dev_EndStateBlock) {
    GUARD;
    StateBlock *b = D->recording;
    if (!b) RETN(2, D3DERR_INVALIDCALL);
    D->recording = nullptr;
    sb_capture(b);
    W32(ARG(1), obj_publish(b, g_vt[K_SB]));
    RETN(2, D3D_OK);
}
M(dev_SetClipStatus) { RETN(2, D3D_OK); }
M(dev_GetClipStatus) { memset(H(ARG(1)), 0, 8); RETN(2, D3D_OK); }
static int stage_index(uint32_t s) { return s < 16 ? (int)s : (s >= 257 && s <= 260) ? (int)(s - 257 + 16) : -1; }
M(dev_GetTexture) {
    GUARD; int i = stage_index(ARG(1));
    Obj *t = i >= 0 ? D->st.tex[i] : nullptr;
    if (t) obj_addref(t);
    W32(ARG(2), t ? t->guest : 0);
    RETN(3, D3D_OK);
}
M(dev_SetTexture) {
    GUARD; int i = stage_index(ARG(1));
    if (i < 0) RETN(3, D3DERR_INVALIDCALL);
    Obj *t = obj_get(ARG(2));
    set_texture(i, t);
    if (D->recording) D->recording->tex[i] = true;
    if (t && t->kind != K_SURF) {
        Texture *tx = static_cast<Texture *>(t);
        if (tx->rendered && tx->autogen && tx->tex) { glBindTexture(tx->target, tx->tex); glGenerateMipmap(tx->target); tx->rendered = false; }
    }
    RETN(3, D3D_OK);
}
M(dev_GetTextureStageState) { GUARD; W32(ARG(3), ARG(1) < 8 && ARG(2) < 33 ? D->st.tss[ARG(1)][ARG(2)] : 0); RETN(4, D3D_OK); }
M(dev_SetTextureStageState) {
    GUARD;
    if (ARG(1) < 8 && ARG(2) < 33) { D->st.tss[ARG(1)][ARG(2)] = ARG(3); if (D->recording) D->recording->tss[ARG(1) * 33 + ARG(2)] = true; }
    RETN(4, D3D_OK);
}
M(dev_GetSamplerState) { GUARD; int i = stage_index(ARG(1)); W32(ARG(3), i >= 0 && ARG(2) < 14 ? D->st.samp[i][ARG(2)] : 0); RETN(4, D3D_OK); }
M(dev_SetSamplerState) {
    GUARD; int i = stage_index(ARG(1));
    if (i >= 0 && ARG(2) < 14) {
        if (D->st.samp[i][ARG(2)] != ARG(3)) { D->st.samp[i][ARG(2)] = ARG(3); D->samp_dirty[i] = true; }
        if (D->recording) D->recording->samp[i * 14 + ARG(2)] = true;
    }
    RETN(4, D3D_OK);
}
M(dev_ValidateDevice) { if (ARG(1)) W32(ARG(1), 1); RETN(2, D3D_OK); }
M(dev_SetPaletteEntries) { RETN(3, D3D_OK); }
M(dev_GetPaletteEntries) { RETN(3, D3D_OK); }
M(dev_SetCurrentTexturePalette) { RETN(2, D3D_OK); }
M(dev_GetCurrentTexturePalette) { W32(ARG(1), 0); RETN(2, D3D_OK); }
M(dev_SetScissorRect) {
    GUARD; uint32_t p = ARG(1);
    for (int i = 0; i < 4; i++) D->st.scissor[i] = (int)R32(p + 4 * i);
    if (D->recording) D->recording->scissor = true;
    D->rs_dirty = true;
    RETN(2, D3D_OK);
}
M(dev_GetScissorRect) { GUARD; for (int i = 0; i < 4; i++) W32(ARG(1) + 4 * i, D->st.scissor[i]); RETN(2, D3D_OK); }
M(dev_SetSoftwareVertexProcessing) { RETN(2, D3D_OK); }
M(dev_GetSoftwareVertexProcessing) { RETN(1, 0); }
M(dev_SetNPatchMode) { RETN(2, D3D_OK); }
M(dev_GetNPatchMode) { RETN(1, 0); }

M(dev_DrawPrimitive) {
    GUARD;
    uint32_t count; GLenum mode = prim_mode(ARG(1), ARG(3), &count);
    if (!count) RETN(4, D3D_OK);
    Program *p = prepare_draw();
    int inst = setup_attribs(current_decl(), p, 0, 0, 0);
    if (inst > 1) glDrawArraysInstanced(mode, ARG(2), count, inst);
    else glDrawArrays(mode, ARG(2), count);
    RETN(4, D3D_OK);
}
M(dev_DrawIndexedPrimitive) {
    GUARD;
    uint32_t count; GLenum mode = prim_mode(ARG(1), ARG(6), &count);
    Buffer *ib = D->st.ib;
    if (!count || !ib) RETN(7, D3D_OK);
    Program *p = prepare_draw();
    int base = (int)ARG(2);
    int inst = setup_attribs(current_decl(), p, 0, 0, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->buf);
    bool i32 = ib->fmt == 102;
    const void *off = (const void *)(uintptr_t)(ARG(5) * (i32 ? 4 : 2));
    GLenum it = i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    if (inst > 1) glDrawElementsInstancedBaseVertex(mode, count, it, off, inst, base);
    else glDrawElementsBaseVertex(mode, count, it, off, base);
    RETN(7, D3D_OK);
}
M(dev_DrawPrimitiveUP) {
    GUARD;
    uint32_t count; GLenum mode = prim_mode(ARG(1), ARG(2), &count);
    uint32_t stride = ARG(4);
    if (!count) RETN(5, D3D_OK);
    set_stream(0, nullptr, 0, 0);
    Program *p = prepare_draw();
    glBindBuffer(GL_ARRAY_BUFFER, D->up_vbo);
    glBufferData(GL_ARRAY_BUFFER, count * stride, H(ARG(3)), GL_STREAM_DRAW);
    setup_attribs(current_decl(), p, D->up_vbo, stride, 0);
    glDrawArrays(mode, 0, count);
    RETN(5, D3D_OK);
}
M(dev_DrawIndexedPrimitiveUP) {
    GUARD;
    uint32_t count; GLenum mode = prim_mode(ARG(1), ARG(4), &count);
    uint32_t minv = ARG(2), nv = ARG(3), stride = ARG(8);
    (void)minv;
    if (!count) RETN(9, D3D_OK);
    set_stream(0, nullptr, 0, 0);
    set_indices(nullptr);
    bool i32 = ARG(6) == 102;
    Program *p = prepare_draw();
    glBindBuffer(GL_ARRAY_BUFFER, D->up_vbo);
    glBufferData(GL_ARRAY_BUFFER, (minv + nv) * stride, H(ARG(7)), GL_STREAM_DRAW);
    setup_attribs(current_decl(), p, D->up_vbo, stride, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, D->up_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, count * (i32 ? 4 : 2), H(ARG(5)), GL_STREAM_DRAW);
    glDrawElements(mode, count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);
    RETN(9, D3D_OK);
}
M(dev_ProcessVertices) { rtlog("d3d9: ProcessVertices not supported"); RETN(7, D3DERR_INVALIDCALL); }
M(dev_CreateVertexDeclaration) {
    GUARD;
    Decl *d = new Decl();
    for (uint32_t p = ARG(1);; p += 8) {
        DeclElem e; memcpy(&e, H(p), 8);
        if (e.stream == 0xFF || e.type == 17) break;
        if (e.usage == 9) d->pretransformed = true;
        d->el.push_back(e);
    }
    W32(ARG(2), obj_publish(d, g_vt[K_DECL]));
    RETN(3, D3D_OK);
}
M(dev_SetVertexDeclaration) { GUARD; set_decl(OBJ<Decl>(ARG(1))); if (D->recording) D->recording->decl = true; RETN(2, D3D_OK); }
M(dev_GetVertexDeclaration) { GUARD; Decl *d = D->st.decl; if (d) obj_addref(d); W32(ARG(1), d ? d->guest : 0); RETN(2, D3D_OK); }
M(dev_SetFVF) {
    GUARD;
    D->st.fvf = ARG(1);
    if (ARG(1)) set_decl(nullptr);
    if (D->recording) D->recording->decl = true;
    RETN(2, D3D_OK);
}
M(dev_GetFVF) { GUARD; W32(ARG(1), D->st.decl ? D->st.decl->fvf : D->st.fvf); RETN(2, D3D_OK); }

static Shader *make_shader(Kind k, uint32_t code) {
    Shader *s = new Shader(k);
    const uint32_t *t = (const uint32_t *)H(code);
    size_t len = sm_length(t, 1 << 20);
    s->code.assign(t, t + len);
    s->ok = sm_translate(s->code.data(), s->code.size(), s->sm);
    if (!s->ok) rtlog("d3d9: shader translation failed (%s): %s", k == K_VS ? "vs" : "ps", s->sm.error.c_str());
    return s;
}
M(dev_CreateVertexShader) { GUARD; Shader *s = make_shader(K_VS, ARG(1)); W32(ARG(2), obj_publish(s, g_vt[K_VS])); RETN(3, D3D_OK); }
M(dev_SetVertexShader) { GUARD; set_shader(false, OBJ<Shader>(ARG(1))); if (D->recording) D->recording->vs = true; RETN(2, D3D_OK); }
M(dev_GetVertexShader) { GUARD; Shader *s = D->st.vs; if (s) obj_addref(s); W32(ARG(1), s ? s->guest : 0); RETN(2, D3D_OK); }
M(dev_SetVertexShaderConstantF) {
    GUARD; uint32_t st = ARG(1), n = ARG(3);
    if (st + n > 256) n = st < 256 ? 256 - st : 0;
    memcpy(D->st.vsf[st], H(ARG(2)), 16 * n);
    if (D->recording) for (uint32_t i = 0; i < n; i++) D->recording->vsf[st + i] = true;
    D->vs_ver++;
    RETN(4, D3D_OK);
}
M(dev_GetVertexShaderConstantF) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 256) memcpy(H(ARG(2)), D->st.vsf[st], 16 * n); RETN(4, D3D_OK); }
M(dev_SetVertexShaderConstantI) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(D->st.vsi[st], H(ARG(2)), 16 * n); if (D->recording) D->recording->vsi = true; D->vs_ver++; RETN(4, D3D_OK); }
M(dev_GetVertexShaderConstantI) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(H(ARG(2)), D->st.vsi[st], 16 * n); RETN(4, D3D_OK); }
M(dev_SetVertexShaderConstantB) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(&D->st.vsb[st], H(ARG(2)), 4 * n); if (D->recording) D->recording->vsb = true; D->vs_ver++; RETN(4, D3D_OK); }
M(dev_GetVertexShaderConstantB) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(H(ARG(2)), &D->st.vsb[st], 4 * n); RETN(4, D3D_OK); }
M(dev_SetStreamSource) {
    GUARD;
    if (ARG(1) >= 16) RETN(5, D3DERR_INVALIDCALL);
    set_stream((int)ARG(1), OBJ<Buffer>(ARG(2)), ARG(3), ARG(4));
    if (D->recording) D->recording->streams = true;
    RETN(5, D3D_OK);
}
M(dev_GetStreamSource) {
    GUARD; StreamSrc &s = D->st.streams[ARG(1) & 15];
    if (s.vb) obj_addref(s.vb);
    W32(ARG(2), s.vb ? s.vb->guest : 0); W32(ARG(3), s.offset); W32(ARG(4), s.stride);
    RETN(5, D3D_OK);
}
M(dev_SetStreamSourceFreq) { GUARD; D->st.streams[ARG(1) & 15].freq = ARG(2); if (D->recording) D->recording->streams = true; RETN(3, D3D_OK); }
M(dev_GetStreamSourceFreq) { GUARD; W32(ARG(2), D->st.streams[ARG(1) & 15].freq); RETN(3, D3D_OK); }
M(dev_SetIndices) { GUARD; set_indices(OBJ<Buffer>(ARG(1))); if (D->recording) D->recording->ib = true; RETN(2, D3D_OK); }
M(dev_GetIndices) { GUARD; Buffer *b = D->st.ib; if (b) obj_addref(b); W32(ARG(1), b ? b->guest : 0); RETN(2, D3D_OK); }
M(dev_CreatePixelShader) { GUARD; Shader *s = make_shader(K_PS, ARG(1)); W32(ARG(2), obj_publish(s, g_vt[K_PS])); RETN(3, D3D_OK); }
M(dev_SetPixelShader) { GUARD; set_shader(true, OBJ<Shader>(ARG(1))); if (D->recording) D->recording->ps = true; RETN(2, D3D_OK); }
M(dev_GetPixelShader) { GUARD; Shader *s = D->st.ps; if (s) obj_addref(s); W32(ARG(1), s ? s->guest : 0); RETN(2, D3D_OK); }
M(dev_SetPixelShaderConstantF) {
    GUARD; uint32_t st = ARG(1), n = ARG(3);
    if (st + n > 224) n = st < 224 ? 224 - st : 0;
    memcpy(D->st.psf[st], H(ARG(2)), 16 * n);
    if (D->recording) for (uint32_t i = 0; i < n; i++) D->recording->psf[st + i] = true;
    D->ps_ver++;
    RETN(4, D3D_OK);
}
M(dev_GetPixelShaderConstantF) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 224) memcpy(H(ARG(2)), D->st.psf[st], 16 * n); RETN(4, D3D_OK); }
M(dev_SetPixelShaderConstantI) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(D->st.psi[st], H(ARG(2)), 16 * n); if (D->recording) D->recording->psi = true; D->ps_ver++; RETN(4, D3D_OK); }
M(dev_GetPixelShaderConstantI) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(H(ARG(2)), D->st.psi[st], 16 * n); RETN(4, D3D_OK); }
M(dev_SetPixelShaderConstantB) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(&D->st.psb[st], H(ARG(2)), 4 * n); if (D->recording) D->recording->psb = true; D->ps_ver++; RETN(4, D3D_OK); }
M(dev_GetPixelShaderConstantB) { GUARD; uint32_t st = ARG(1), n = ARG(3); if (st + n <= 16) memcpy(H(ARG(2)), &D->st.psb[st], 4 * n); RETN(4, D3D_OK); }
M(dev_DrawRectPatch) { RETN(4, D3DERR_INVALIDCALL); }
M(dev_DrawTriPatch) { RETN(4, D3DERR_INVALIDCALL); }
M(dev_DeletePatch) { RETN(2, D3D_OK); }
M(dev_CreateQuery) {
    GUARD;
    uint32_t type = ARG(1);
    if (type != 8 && type != 9) { if (ARG(2)) W32(ARG(2), 0); RETN(3, D3DERR_NOTAVAILABLE); }
    if (!ARG(2)) RETN(3, D3D_OK);
    Query *q = new Query(); q->type = type;
    W32(ARG(2), obj_publish(q, g_vt[K_QUERY]));
    RETN(3, D3D_OK);
}

// swap chain
M(sw_Present) { GUARD; present(); RETN(6, D3D_OK); }
M(sw_GetFrontBufferData) { RETN(2, D3D_OK); }
M(sw_GetBackBuffer) { GUARD; W32(ARG(3), D->backbuf->guest); obj_addref(D->backbuf); RETN(4, D3D_OK); }
M(sw_GetRasterStatus) { W32(ARG(1), 0); W32(ARG(1) + 4, 0); RETN(2, D3D_OK); }
M(sw_GetDisplayMode) { write_display_mode(ARG(1)); RETN(2, D3D_OK); }
M(sw_GetPresentParameters) { GUARD; for (int i = 0; i < 14; i++) W32(ARG(1) + 4 * i, D->pp[i]); RETN(2, D3D_OK); }

// ====================================================================== IDirect3D9
struct ModeList { std::vector<std::pair<int, int>> m; };
static ModeList &modes() {
    static ModeList ml;
    if (ml.m.empty()) {
        ui_ensure_sdl();
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        int dw = dm ? dm->w : 1920, dh = dm ? dm->h : 1080;
        static const int common[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800},
            {1280, 960}, {1280, 1024}, {1360, 768}, {1366, 768}, {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050},
            {1920, 1080}, {1920, 1200}, {2048, 1152}, {2560, 1440}, {2560, 1600}};
        for (auto &c2 : common) if (c2[0] <= dw && c2[1] <= dh) ml.m.push_back({c2[0], c2[1]});
        if (std::find(ml.m.begin(), ml.m.end(), std::make_pair(dw, dh)) == ml.m.end()) ml.m.push_back({dw, dh});
    }
    return ml;
}
M(d3d_RegisterSoftwareDevice) { RETN(2, D3DERR_INVALIDCALL); }
M(d3d_GetAdapterCount) { RETN(1, 1); }
M(d3d_GetAdapterIdentifier) {
    uint32_t p = ARG(3);
    memset(H(p), 0, 1100);
    gstrcpy(p, "aticfx32.dll", 512);
    gstrcpy(p + 512, "ATI Radeon HD 4800 Series", 512);
    gstrcpy(p + 1024, "\\\\.\\DISPLAY1", 32);
    W32(p + 1056, 0x000C0000 | 1); W32(p + 1060, 0x00080000 | 0x0F);   // driver version 8.15.x
    W32(p + 1064, 0x1002); W32(p + 1068, 0x9442); W32(p + 1072, 0); W32(p + 1076, 0);
    W32(p + 1080, 0xD7B71EE2); W32(p + 1084, 0x11CF4D02); W32(p + 1088, 0x6F21DD9A); W32(p + 1092, 0x35E8C5E3);
    W32(p + 1096, 1);
    RETN(4, D3D_OK);
}
M(d3d_GetAdapterModeCount) { RETN(3, ARG(2) == 22 || ARG(2) == 21 ? (uint32_t)modes().m.size() : 0); }
M(d3d_EnumAdapterModes) {
    auto &m = modes().m;
    if (ARG(3) >= m.size()) RETN(5, D3DERR_INVALIDCALL);
    uint32_t p = ARG(4);
    W32(p, m[ARG(3)].first); W32(p + 4, m[ARG(3)].second); W32(p + 8, 60); W32(p + 12, ARG(2));
    RETN(5, D3D_OK);
}
M(d3d_GetAdapterDisplayMode) { write_display_mode(ARG(2)); RETN(3, D3D_OK); }
M(d3d_CheckDeviceType) { RETN(6, D3D_OK); }
M(d3d_CheckDeviceFormat) {
    uint32_t usage = ARG(4), rtype = ARG(5), fmt = ARG(6);
    const FmtInfo *fi = fmt_info(fmt);
    uint32_t r = D3D_OK;
    if (!fi) r = D3DERR_NOTAVAILABLE;
    else if ((usage & 2) && !fi->depth) r = D3DERR_NOTAVAILABLE;         // DEPTHSTENCIL
    else if ((usage & 1) && (fi->compressed || fi->depth)) r = D3DERR_NOTAVAILABLE;   // RENDERTARGET
    else if (fi->depth && rtype == 3 && !(fmt == FOURCC('I','N','T','Z') || fmt == FOURCC('D','F','2','4') || fmt == FOURCC('D','F','1','6'))) r = D3DERR_NOTAVAILABLE;
    else if (usage & 0x00080000) r = D3DERR_NOTAVAILABLE;   // QUERY_VERTEXTEXTURE on exotic formats: keep it simple
    if (fmt == 32 || fmt == 33) r = D3DERR_NOTAVAILABLE;      // A8B8G8R8 not exposed by D3D9 ATI drivers
    if (trace() || r) rtlog("d3d9: CheckDeviceFormat usage %x rtype %u fmt %08x -> %s", usage, rtype, fmt, r ? "no" : "ok");
    RETN(7, r);
}
M(d3d_CheckDeviceMultiSampleType) {
    uint32_t ms = ARG(5);
    if (ARG(6)) W32(ARG(6), ms == 0 ? 1 : 0);
    RETN(7, ms == 0 ? D3D_OK : D3DERR_NOTAVAILABLE);
}
M(d3d_CheckDepthStencilMatch) { RETN(6, D3D_OK); }
M(d3d_CheckDeviceFormatConversion) { RETN(5, D3D_OK); }
M(d3d_GetDeviceCaps) { fill_caps(ARG(3)); RETN(4, D3D_OK); }
M(d3d_GetAdapterMonitor) { RETN(2, 0x00C20001); }

static void init_vtables();
M(d3d_CreateDevice) {
    GUARD;
    if (D) { rtlog("d3d9: second device not supported"); RETN(7, D3DERR_NOTAVAILABLE); }
    uint32_t hfocus = ARG(3), behavior = ARG(4), pp = ARG(5);
    D = new Device();
    D->d3d = ARG(0);
    D->behavior = behavior;
    for (int i = 0; i < 14; i++) D->pp[i] = R32(pp + 4 * i);
    D->hwnd = D->pp[7] ? D->pp[7] : hfocus;
    g_win = ui_sdl_window(D->hwnd);
    if (!g_win) { rtlog("d3d9: CreateDevice without a window"); RETN(7, D3DERR_INVALIDCALL); }
    if (!D->pp[0] || !D->pp[1]) {
        int w, h; SDL_GetWindowSize(g_win, &w, &h);
        D->pp[0] = w; D->pp[1] = h; W32(pp, w); W32(pp + 4, h);
    }
    if (!D->pp[2]) { D->pp[2] = 22; W32(pp + 8, 22); }
    if (!D->pp[3]) { D->pp[3] = 1; W32(pp + 12, 1); }
    rtlog("d3d9: CreateDevice %ux%u fmt %u windowed=%u depth=%u/%u interval=%x behavior=%x",
          D->pp[0], D->pp[1], D->pp[2], D->pp[8], D->pp[9], D->pp[10], D->pp[13], behavior);
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) { rtlog("d3d9: GL context creation failed: %s", SDL_GetError()); RETN(7, D3DERR_NOTAVAILABLE); }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    g_ctx_thread = pthread_self(); g_ctx_bound = true;
    rtlog("d3d9: OpenGL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glGenVertexArrays(1, &D->vao);
    glBindVertexArray(D->vao);
    glGenBuffers(1, &D->up_vbo);
    glGenBuffers(1, &D->up_ibo);
    glGenSamplers(20, D->samplers);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    Obj *dev = new Obj(K_DEVICE);
    D->guest = obj_publish(dev, g_vt[K_DEVICE]);
    Obj *sw = new Obj(K_SWAP);
    D->swap = obj_publish(sw, g_vt[K_SWAP]);
    obj_addref(obj_get(D->d3d));
    reset_state();
    create_backbuffer();
    apply_window_mode();
    W32(ARG(6), D->guest);
    RETN(7, D3D_OK);
}

// ====================================================================== vtables
static void init_vtables() {
    if (g_vt[K_D3D]) return;
    {
        HostFn m[17] = {gen_QueryInterface, gen_AddRef, gen_Release, d3d_RegisterSoftwareDevice, d3d_GetAdapterCount,
            d3d_GetAdapterIdentifier, d3d_GetAdapterModeCount, d3d_EnumAdapterModes, d3d_GetAdapterDisplayMode,
            d3d_CheckDeviceType, d3d_CheckDeviceFormat, d3d_CheckDeviceMultiSampleType, d3d_CheckDepthStencilMatch,
            d3d_CheckDeviceFormatConversion, d3d_GetDeviceCaps, d3d_GetAdapterMonitor, d3d_CreateDevice};
        g_vt[K_D3D] = com_make_vtable("IDirect3D9", m, 17);
    }
    {
        HostFn m[119] = {gen_QueryInterface, gen_AddRef, gen_Release, dev_TestCooperativeLevel, dev_GetAvailableTextureMem,
            dev_EvictManagedResources, dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters,
            dev_SetCursorProperties, dev_SetCursorPosition, dev_ShowCursor, dev_CreateAdditionalSwapChain, dev_GetSwapChain,
            dev_GetNumberOfSwapChains, dev_Reset, dev_Present, dev_GetBackBuffer, dev_GetRasterStatus, dev_SetDialogBoxMode,
            dev_SetGammaRamp, dev_GetGammaRamp, dev_CreateTexture, dev_CreateVolumeTexture, dev_CreateCubeTexture,
            dev_CreateVertexBuffer, dev_CreateIndexBuffer, dev_CreateRenderTarget, dev_CreateDepthStencilSurface,
            dev_UpdateSurface, dev_UpdateTexture, dev_GetRenderTargetData, dev_GetFrontBufferData, dev_StretchRect,
            dev_ColorFill, dev_CreateOffscreenPlainSurface, dev_SetRenderTarget, dev_GetRenderTarget,
            dev_SetDepthStencilSurface, dev_GetDepthStencilSurface, dev_BeginScene, dev_EndScene, dev_Clear,
            dev_SetTransform, dev_GetTransform, dev_MultiplyTransform, dev_SetViewport, dev_GetViewport, dev_SetMaterial,
            dev_GetMaterial, dev_SetLight, dev_GetLight, dev_LightEnable, dev_GetLightEnable, dev_SetClipPlane,
            dev_GetClipPlane, dev_SetRenderState, dev_GetRenderState, dev_CreateStateBlock, dev_BeginStateBlock,
            dev_EndStateBlock, dev_SetClipStatus, dev_GetClipStatus, dev_GetTexture, dev_SetTexture,
            dev_GetTextureStageState, dev_SetTextureStageState, dev_GetSamplerState, dev_SetSamplerState,
            dev_ValidateDevice, dev_SetPaletteEntries, dev_GetPaletteEntries, dev_SetCurrentTexturePalette,
            dev_GetCurrentTexturePalette, dev_SetScissorRect, dev_GetScissorRect, dev_SetSoftwareVertexProcessing,
            dev_GetSoftwareVertexProcessing, dev_SetNPatchMode, dev_GetNPatchMode, dev_DrawPrimitive,
            dev_DrawIndexedPrimitive, dev_DrawPrimitiveUP, dev_DrawIndexedPrimitiveUP, dev_ProcessVertices,
            dev_CreateVertexDeclaration, dev_SetVertexDeclaration, dev_GetVertexDeclaration, dev_SetFVF, dev_GetFVF,
            dev_CreateVertexShader, dev_SetVertexShader, dev_GetVertexShader, dev_SetVertexShaderConstantF,
            dev_GetVertexShaderConstantF, dev_SetVertexShaderConstantI, dev_GetVertexShaderConstantI,
            dev_SetVertexShaderConstantB, dev_GetVertexShaderConstantB, dev_SetStreamSource, dev_GetStreamSource,
            dev_SetStreamSourceFreq, dev_GetStreamSourceFreq, dev_SetIndices, dev_GetIndices, dev_CreatePixelShader,
            dev_SetPixelShader, dev_GetPixelShader, dev_SetPixelShaderConstantF, dev_GetPixelShaderConstantF,
            dev_SetPixelShaderConstantI, dev_GetPixelShaderConstantI, dev_SetPixelShaderConstantB,
            dev_GetPixelShaderConstantB, dev_DrawRectPatch, dev_DrawTriPatch, dev_DeletePatch, dev_CreateQuery};
        g_vt[K_DEVICE] = com_make_vtable("IDirect3DDevice9", m, 119);
    }
    HostFn res[11] = {gen_QueryInterface, gen_AddRef, gen_Release, res_GetDevice, res_SetPrivateData, res_GetPrivateData,
                      res_FreePrivateData, res_SetPriority, res_GetPriority, res_PreLoad, res_GetType};
    auto with_res = [&](std::initializer_list<HostFn> extra, const char *name) {
        std::vector<HostFn> v(res, res + 11);
        v.insert(v.end(), extra);
        return com_make_vtable(name, v.data(), (int)v.size());
    };
    g_vt[K_TEX] = with_res({tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_SetAutoGenFilterType, tex_GetAutoGenFilterType,
                            tex_GenerateMipSubLevels, tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect, tex_AddDirtyRect}, "IDirect3DTexture9");
    g_vt[K_CUBE] = with_res({tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_SetAutoGenFilterType, tex_GetAutoGenFilterType,
                             tex_GenerateMipSubLevels, cube_GetLevelDesc, cube_GetCubeMapSurface, cube_LockRect, cube_UnlockRect, cube_AddDirtyRect}, "IDirect3DCubeTexture9");
    g_vt[K_VOL] = with_res({tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_SetAutoGenFilterType, tex_GetAutoGenFilterType,
                            tex_GenerateMipSubLevels, vol_GetLevelDesc, nullptr, vol_LockBox, vol_UnlockBox, tex_AddDirtyRect}, "IDirect3DVolumeTexture9");
    g_vt[K_SURF] = with_res({surf_GetContainer, surf_GetDesc, surf_LockRect, surf_UnlockRect, surf_GetDC, surf_ReleaseDC}, "IDirect3DSurface9");
    g_vt[K_VB] = with_res({buf_Lock, buf_Unlock, vb_GetDesc}, "IDirect3DVertexBuffer9");
    g_vt[K_IB] = with_res({buf_Lock, buf_Unlock, ib_GetDesc}, "IDirect3DIndexBuffer9");
    { HostFn m[5] = {gen_QueryInterface, gen_AddRef, gen_Release, res_GetDevice, decl_GetDeclaration}; g_vt[K_DECL] = com_make_vtable("IDirect3DVertexDeclaration9", m, 5); }
    { HostFn m[5] = {gen_QueryInterface, gen_AddRef, gen_Release, res_GetDevice, sh_GetFunction}; g_vt[K_VS] = com_make_vtable("IDirect3DVertexShader9", m, 5); }
    { HostFn m[5] = {gen_QueryInterface, gen_AddRef, gen_Release, res_GetDevice, sh_GetFunction}; g_vt[K_PS] = com_make_vtable("IDirect3DPixelShader9", m, 5); }
    { HostFn m[8] = {gen_QueryInterface, gen_AddRef, gen_Release, res_GetDevice, q_GetType, q_GetDataSize, q_Issue, q_GetData}; g_vt[K_QUERY] = com_make_vtable("IDirect3DQuery9", m, 8); }
    { HostFn m[6] = {gen_QueryInterface, gen_AddRef, gen_Release, res_GetDevice, sb_Capture, sb_Apply}; g_vt[K_SB] = com_make_vtable("IDirect3DStateBlock9", m, 6); }
    { HostFn m[10] = {gen_QueryInterface, gen_AddRef, gen_Release, sw_Present, sw_GetFrontBufferData, sw_GetBackBuffer,
                      sw_GetRasterStatus, sw_GetDisplayMode, res_GetDevice, sw_GetPresentParameters}; g_vt[K_SWAP] = com_make_vtable("IDirect3DSwapChain9", m, 10); }
}

API(d3d9, Direct3DCreate9, 1) {
    GUARD;
    init_vtables();
    ui_ensure_sdl();
    Obj *o = new Obj(K_D3D);
    uint32_t g = obj_publish(o, g_vt[K_D3D]);
    rtlog("Direct3DCreate9(%u) -> %08x", ARG(0), g);
    RETN(1, g);
}

uint32_t d3d9_surface_read(uint32_t surf_guest, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    D3DGuard g;
    Surface *s = OBJ<Surface>(surf_guest);
    if (!s) return D3DERR_INVALIDCALL;
    w = s->w; h = s->h;
    rgba.resize((size_t)w * h * 4);
    uint32_t pitch, m = surface_cpu(s, &pitch);
    const FmtInfo *fi = fmt_info(s->fmt);
    if (m && fi && fi->format == GL_BGRA && fi->bpp == 4) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = H(m + y * pitch + x * 4);
            uint8_t *q = &rgba[((size_t)y * w + x) * 4];
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = s->fmt == 22 ? 255 : p[3];
        }
        return D3D_OK;
    }
    if (s->gl_tex()) {
        GLuint f = fbo_for(&s, 1, nullptr);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        D->fbo_dirty = true;
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

// ====================================================================== RGBA8 access for the D3DX helpers
static void decode_px(uint32_t fmt, const uint8_t *p, uint8_t *o) {
    switch (fmt) {
    case 21: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;
    case 22: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
    case 32: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
    case 23: { uint16_t v = p[0] | (p[1] << 8); o[0] = ((v >> 11) & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = (v & 31) * 255 / 31; o[3] = 255; break; }
    case 24: case 25: { uint16_t v = p[0] | (p[1] << 8); o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = fmt == 24 || (v >> 15) ? 255 : 0; break; }
    case 26: { uint16_t v = p[0] | (p[1] << 8); o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; o[3] = (v >> 12) * 17; break; }
    case 50: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
    case 28: o[0] = o[1] = o[2] = 0; o[3] = p[0]; break;
    case 51: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;
    default: o[0] = o[1] = o[2] = 0; o[3] = 255; break;
    }
}
static void encode_px(uint32_t fmt, const uint8_t *i, uint8_t *p) {
    switch (fmt) {
    case 21: case 22: p[0] = i[2]; p[1] = i[1]; p[2] = i[0]; p[3] = fmt == 22 ? 255 : i[3]; break;
    case 32: case 33: p[0] = i[0]; p[1] = i[1]; p[2] = i[2]; p[3] = i[3]; break;
    case 23: { uint16_t v = (uint16_t)(((i[0] >> 3) << 11) | ((i[1] >> 2) << 5) | (i[2] >> 3)); p[0] = v & 255; p[1] = v >> 8; break; }
    case 24: case 25: { uint16_t v = (uint16_t)(((i[3] >= 128) << 15) | ((i[0] >> 3) << 10) | ((i[1] >> 3) << 5) | (i[2] >> 3)); p[0] = v & 255; p[1] = v >> 8; break; }
    case 26: { uint16_t v = (uint16_t)(((i[3] >> 4) << 12) | ((i[0] >> 4) << 8) | ((i[1] >> 4) << 4) | (i[2] >> 4)); p[0] = v & 255; p[1] = v >> 8; break; }
    case 50: p[0] = (uint8_t)((i[0] * 77 + i[1] * 150 + i[2] * 29) >> 8); break;
    case 28: p[0] = i[3]; break;
    case 51: p[0] = (uint8_t)((i[0] * 77 + i[1] * 150 + i[2] * 29) >> 8); p[1] = i[3]; break;
    default: break;
    }
}

// read a rectangle of a surface as RGBA8 (rows top to bottom)
bool d3d9_get_rgba(uint32_t surf, int x0, int y0, int w, int h, std::vector<uint8_t> &out) {
    D3DGuard g;
    Surface *s = OBJ<Surface>(surf);
    if (!s || w <= 0 || h <= 0) return false;
    out.assign((size_t)w * h * 4, 0);
    const FmtInfo *fi = fmt_info(s->fmt);
    uint32_t pitch, m = surface_cpu(s, &pitch);
    if (m && fi && !fi->compressed) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
            decode_px(s->fmt, H(m + (y0 + y) * pitch + (x0 + x) * fi->bpp), &out[((size_t)y * w + x) * 4]);
        return true;
    }
    if (s->gl_tex() && fi && !fi->depth) {
        GLuint f = fbo_for(&s, 1, nullptr);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        D->fbo_dirty = true;
        return true;
    }
    return false;
}
// write RGBA8 into a rectangle of a surface (converting to its format)
bool d3d9_put_rgba(uint32_t surf, int x0, int y0, int w, int h, const std::vector<uint8_t> &in) {
    D3DGuard g;
    Surface *s = OBJ<Surface>(surf);
    if (!s || w <= 0 || h <= 0) return false;
    const FmtInfo *fi = fmt_info(s->fmt);
    if (!fi || fi->depth) return false;
    if (s->gl_tex() && !s->mem && !(s->container && s->container->sysmem())) {
        if (fi->compressed) { rtlog("d3dx: writing RGBA into a compressed GL surface is not supported"); return false; }
        glBindTexture(s->container && s->container->kind == K_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, s->gl_tex());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(s->gl_target(), s->level, x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, in.data());
        return true;
    }
    uint32_t pitch;
    uint32_t m = s->container ? lock_level(s->container, s->face, s->level, 0, 0, &pitch, nullptr) : surf_mem(s, &pitch, true);
    if (fi->compressed) return false;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        encode_px(s->fmt, &in[((size_t)y * w + x) * 4], H(m + (y0 + y) * pitch + (x0 + x) * fi->bpp));
    if (s->container) unlock_level(s->container, s->face, s->level);
    return true;
}
bool d3d9_surface_size(uint32_t surf, uint32_t &w, uint32_t &h) {
    D3DGuard g;
    Surface *s = OBJ<Surface>(surf);
    if (!s) return false;
    w = s->w; h = s->h;
    return true;
}
