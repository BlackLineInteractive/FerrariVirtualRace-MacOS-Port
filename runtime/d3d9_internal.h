// Internal structures of the Direct3D 9 -> OpenGL 4.1 layer.
#pragma once
#define GL_SILENCE_DEPRECATION
#include "rt.h"
#include "d3d9_shader.h"
#include <OpenGL/gl3.h>
#include <SDL3/SDL.h>
#include <map>
#include <vector>
#include <string>
#include <memory>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum : uint32_t {
    D3D_OK = 0, D3DERR_INVALIDCALL = 0x8876086C, D3DERR_NOTAVAILABLE = 0x8876086A,
    D3DERR_NOTFOUND = 0x88760866, D3DERR_DEVICELOST = 0x88760868, D3DERR_DEVICENOTRESET = 0x88760869,
    D3DERR_OUTOFVIDEOMEMORY = 0x8876017C, E_NOINTERFACE = 0x80004002, E_FAIL = 0x80004005, S_FALSE = 1,
};

// ---------------------------------------------------------------- formats
struct FmtInfo {
    uint32_t d3d;
    GLenum internal, format, type;
    int bpp;              // bytes per pixel, or bytes per 4x4 block when compressed
    bool compressed, depth, stencil;
    GLint swz[4];         // texture swizzle
};
const FmtInfo *fmt_info(uint32_t d3dfmt);
uint32_t fmt_pitch(const FmtInfo *f, uint32_t w);           // bytes per row (or per block row)
uint32_t fmt_rows(const FmtInfo *f, uint32_t h);            // rows (or block rows)

// ---------------------------------------------------------------- objects
enum Kind { K_D3D, K_DEVICE, K_TEX, K_CUBE, K_VOL, K_SURF, K_VB, K_IB, K_DECL, K_VS, K_PS, K_QUERY, K_SB, K_SWAP };

struct Obj {
    uint32_t guest = 0;
    int refs = 1;
    Kind kind;
    explicit Obj(Kind k) : kind(k) {}
    virtual ~Obj() {}
};

struct Texture;
struct Surface : Obj {
    Surface() : Obj(K_SURF) {}
    Texture *container = nullptr;   // owning texture (refcount forwarded), or null for standalone surfaces
    int level = 0, face = 0;
    uint32_t w = 0, h = 0, fmt = 0, usage = 0, pool = 0, ms = 0;
    GLuint tex = 0;                 // standalone render target / depth surfaces own a GL texture
    GLuint rb = 0;                  // or a renderbuffer (multisampled)
    // MSAA shadow (d3d9.cpp): a multisampled renderbuffer drawn instead of the texture; resolved
    // into it when the game switches away. ms_dirty: renderbuffer newer; tex_dirty: texture newer.
    GLuint ms_rb = 0, ms_fbo = 0;
    int ms_n = 0;
    bool ms_dirty = false, tex_dirty = false;
    uint32_t mem = 0;               // guest memory for locks / sysmem surfaces
    uint32_t lock_flags = 0;
    bool locked = false;
    GLuint gl_tex() const;
    GLenum gl_target() const;
    ~Surface();
};

struct Texture : Obj {
    explicit Texture(Kind k) : Obj(k) {}
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    uint32_t w = 0, h = 0, d = 1, levels = 1, fmt = 0, usage = 0, pool = 0;
    std::vector<uint32_t> mem;            // per (face*levels+level) guest buffer, 0 if none
    std::vector<Surface *> surfs;         // per (face*levels+level)
    uint32_t lod = 0;
    bool autogen = false;
    bool rendered = false;      // rendered to since mips were last generated
    bool shadow = false;        // depth format sampled as a depth comparison (D16 / D24X8 / D24S8 ...)
    bool sysmem() const { return pool == 2 || pool == 3; }
    ~Texture();
};

struct Buffer : Obj {
    explicit Buffer(Kind k) : Obj(k) {}
    GLuint buf = 0;
    uint32_t size = 0, usage = 0, fmt = 0, pool = 0, fvf = 0;
    uint32_t mem = 0;           // guest shadow copy (always present)
    uint32_t lock_off = 0, lock_size = 0, lock_flags = 0;
    int locks = 0;
    uint32_t dirty_lo = ~0u, dirty_hi = 0;
    ~Buffer();
};

struct DeclElem { uint16_t stream, offset; uint8_t type, method, usage, index; };
struct Decl : Obj {
    Decl() : Obj(K_DECL) {}
    std::vector<DeclElem> el;
    uint32_t fvf = 0;           // non-zero if generated from an FVF
    bool pretransformed = false;
    uint32_t guest_copy = 0;
};

struct Shader : Obj {
    explicit Shader(Kind k) : Obj(k) {}
    SmShader sm;
    bool ok = false;
    uint32_t uid = 0;           // unique for the process lifetime (program cache key)
    uint64_t hash = 0;          // of the bytecode (persistent shader-pair cache)
    ~Shader();
    std::vector<uint32_t> code;
};

struct Query : Obj {
    Query() : Obj(K_QUERY) {}
    uint32_t type = 0;
    GLuint q = 0;
    bool issued = false;
};

// ---------------------------------------------------------------- device state
struct Light {
    uint32_t type; float diffuse[4], specular[4], ambient[4], pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
};
struct Material { float diffuse[4], ambient[4], specular[4], emissive[4], power; };

struct Viewport { uint32_t x, y, w, h; float minz, maxz; };

struct StreamSrc { Buffer *vb = nullptr; uint32_t offset = 0, stride = 0, freq = 1; };

struct DevState {
    uint32_t rs[256];
    uint32_t tss[8][33];
    uint32_t samp[20][14];
    Obj *tex[20] = {};          // Texture* (0..15 PS, 16..19 VS)
    float vsf[256][4], psf[224][4];
    int vsi[16][4], psi[16][4];
    int vsb[16], psb[16];
    float transform[512][16];   // indexed by D3DTRANSFORMSTATETYPE (world = 256+)
    Light lights[8];
    bool light_on[8];
    Material mat;
    Viewport vp;
    int scissor[4];
    float clip[8][4];
    StreamSrc streams[16];
    Buffer *ib = nullptr;
    Decl *decl = nullptr;
    uint32_t fvf = 0;
    Shader *vs = nullptr, *ps = nullptr;
};

void d3d_lock();
void d3d_unlock();
struct D3DGuard { D3DGuard() { d3d_lock(); } ~D3DGuard() { d3d_unlock(); } };

// object registry
Obj *obj_get(uint32_t guest);
template <class T> static inline T *OBJ(uint32_t g) { return static_cast<T *>(obj_get(g)); }
uint32_t obj_publish(Obj *o, uint32_t vtable);   // creates the guest COM object
void obj_addref(Obj *o);
uint32_t obj_release(Obj *o);

// fixed function shader generation
struct FFKey;
std::string ff_vs_source(const DevState &st, const Decl *decl, const SmShader *ps, std::string &key);
std::string ff_ps_source(const DevState &st, const SmShader *vs, std::string &key);
SmShader ff_ps_iface();        // varyings read by the fixed-function pixel stage
SmShader ff_vs_iface(const Decl *decl, const DevState &st);
int ff_attrib_location(int usage, int index);

// D3DX-like helpers used by the d3dx9 replacement
uint32_t d3d9_surface_read(uint32_t surf_guest, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h);
