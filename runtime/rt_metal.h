// Metal compute next to the OpenGL renderer: traces rays through the tinybvh acceleration
// structures built by rt_scene.cpp and hands the result to OpenGL through an IOSurface.
#pragma once
#include <cstddef>
#include <cstdint>

// One TLAS leaf: world -> object (3 rows of a 3x4 matrix) and where its CWBVH lives in the node
// and triangle buffers (in float4 units). 64 bytes, as the kernel reads it.
struct RtmInst {
    float w2o[12];
    uint32_t node_base, tri_base, tint, pad;
};

bool rtm_available();
// BLAS data (float4 counts). Everything from `from_*` on is new since the last call; 0 = all of it.
void rtm_blas(const void *nodes, size_t n_nodes, size_t from_nodes, const void *tris, size_t n_tris, size_t from_tris);
// The merged world-space BLAS (RtmInst::pad = 1 selects it), replacing the previous one.
void rtm_merged(const void *nodes, size_t n_nodes, const void *tris, size_t n_tris);
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
// Returns the GL rectangle texture of hits: (position, 1) hit, (0, 0, 0, 2) miss, 0 not traced.
unsigned rtm_trace_reflect(const RtmInst *inst, size_t n_inst, const void *tlas_nodes, size_t n_tlas, const uint32_t *prim, size_t n_prim,
                           const float *cam, uint32_t *rays, double *gpu_ms);
