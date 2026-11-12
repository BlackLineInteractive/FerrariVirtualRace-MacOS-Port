// Metal compute next to the OpenGL renderer: traces rays through the tinybvh acceleration
// structures built by rt_scene.cpp and hands the result to OpenGL through an IOSurface.
#pragma once
#include <cstddef>
#include <cstdint>

// One TLAS leaf: world -> object (3 rows of a 3x4 matrix), where its CWBVH and triangle
// attributes live (node / triangle buffers in float4 units, attributes in records) and its
// material. 128 bytes, as the kernel reads it. flags: 1 merged track (its own buffers, per-triangle
// textures), 2 car material (hemisphere ambient), 4 texture matrix in uvm, 8 glass.
struct RtmInst {
    float w2o[12];
    uint32_t node_base, tri_base, attr_base, flags;
    float color[4];
    float uvm[8];
    int32_t tex;
    uint32_t pad[3];
};
// Light for shading the hits (the game's constants).
struct RtmLight {
    float sun_dir[4], sun_col[4], amb_lo[4], amb_hi[4];
    float amb_level, pad[3];
};

bool rtm_available();
// BLAS data (float4 counts). Everything from `from_*` on is new since the last call; 0 = all of it.
void rtm_blas(const void *nodes, size_t n_nodes, size_t from_nodes, const void *tris, size_t n_tris, size_t from_tris);
// The merged world-space BLAS (RtmInst::pad = 1 selects it), replacing the previous one.
void rtm_merged(const void *nodes, size_t n_nodes, const void *tris, size_t n_tris, const void *attr, size_t n_attr);
// Triangle attribute records (40 bytes each) of every BLAS; from `from` on is new (0: all).
void rtm_attrs(const void *attr, size_t n, size_t from);
// Texels (RGBA8 words) and texture infos (offset, w, h, pad); texels from `from_texels` on are new.
void rtm_textures(const uint32_t *texels, size_t n_texels, size_t from_texels, const void *info, size_t n_info);
// Traces one primary ray per pixel from the camera (ivp: inverse view-projection, rows) into a
// w x h RGBA16F image shared with OpenGL and waits for it. tlas_nodes: tinybvh BVH nodes (32 bytes),
// prim: their leaf instance indices. Returns the GL_TEXTURE_RECTANGLE texture, 0 on failure.
unsigned rtm_trace_view(const RtmInst *inst, size_t n_inst, const void *tlas_nodes, size_t n_tlas, const uint32_t *prim, size_t n_prim,
                        const float *ivp, int w, int h, int mode, double *gpu_ms);

// Reflection mask images (w x h, the scene's render size) that OpenGL renders into as colour
// attachments: m0 RGBA16F = (reflection direction, GGX alpha), m1 RGBA32F = (position - camera,
// weight; 0 = no ray). False when Metal is unavailable.
bool rtm_mask_textures(int w, int h, unsigned *gl_m0, unsigned *gl_m1);
// Traces one reflection ray for every pixel of the mask with a weight > 0 and waits for it.
// Returns the GL rectangle texture of hits: (position, 1) hit, (0, 0, 0, 2) miss, 0 not traced;
// the shaded colour of each hit is in rtm_hit_colour().
unsigned rtm_trace_reflect(const RtmInst *inst, size_t n_inst, const void *tlas_nodes, size_t n_tlas, const uint32_t *prim, size_t n_prim,
                           const float *cam, const RtmLight &light, bool refract, uint32_t *rays, double *gpu_ms);
// refract: also trace through glass (rt_refract=1). rays[4]: reflection rays, glass pixels, glass pixels whose refracted ray met a second glass
// surface, and the sum of those distances x 1000.
unsigned rtm_hit_colour();
// Glass: transmittance x (refracted - straight-through colour), to add to the frame.
unsigned rtm_glass();
