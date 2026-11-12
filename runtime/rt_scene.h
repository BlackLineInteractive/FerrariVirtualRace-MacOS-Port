// Ray-tracing scene: the game's 3D scene as tinybvh acceleration structures, gathered from the
// draws of the scene pass (d3d9.cpp).
//
//   BLAS: one CWBVH (BVH8) per distinct mesh (vertex/index buffer contents and draw range), built
//         the first time it is drawn and kept while it is in use.
//   TLAS: one BVH2 over this frame's draws (instances: BLAS + object-to-world matrix), rebuilt at
//         the end of every frame.
#pragma once
#include <cstdint>

struct RtDraw {
    int prim;                    // D3DPRIMITIVETYPE: 4 list, 5 strip
    uint32_t prims;              // primitive count
    uint32_t first;              // first vertex (non-indexed) or first index (indexed)
    int32_t base_vertex;         // added to every index
    const uint8_t *ib;           // index data (null: non-indexed)
    bool i32;
    const uint8_t *pos;          // position of vertex 0: vertex buffer + stream offset + element offset
    uint32_t stride;
    uint8_t pos_type;            // D3DDECLTYPE of the position (2: FLOAT3, 3: FLOAT4)
    uint32_t vb_size;            // bytes available from `pos`
    float m[16];                 // object -> world (3 rows, wvp false) or object -> clip (wvp true)
    bool wvp;                    // m is WorldViewProjMtx: world = inverse(camera) * m, at the end of the frame
    uint32_t vb_id, vb_ver;      // identify the geometry: guest address of `pos` and buffer versions
    uint32_t ib_id, ib_ver;
    const char *shader;          // vertex shader's constant names (statistics)
    bool fixed;                  // world-space track geometry: cannot move
};

// Why a draw was left out (statistics).
enum RtSkip { RT_SKIP_SHADER, RT_SKIP_INSTANCED, RT_SKIP_UP, RT_SKIP_FORMAT, RT_SKIP_PRIM, RT_SKIP_VOLATILE,
              RT_SKIP_BUDGET, RT_SKIP_EMPTY, RT_SKIP_N };

// enabled: ray tracing is on (FVR_RT / options.txt rt=1); FVR_RT_DUMP also captures one frame.
bool rt_capture_active(uint64_t frame, bool enabled);
void rt_capture(const RtDraw &d);
void rt_skip(RtSkip why, const char *what = nullptr);
// End of the frame: vp = camera view-projection (rows = clip components), ivp its inverse.
void rt_frame_end(uint64_t frame, const float *vp, const float *ivp);
// Builds this frame's TLAS now and traces it on the GPU (Metal): returns an OpenGL rectangle
// texture (w x h, RGBA16F) with the debug view, or 0.
// mode 1: the camera's view, 2: looking backwards from the camera
unsigned rt_trace_view(uint64_t frame, const float *vp, const float *ivp, int w, int h, int mode);
// Reflection rays for the pixels the PBR car shader marked (rtm_mask_textures), from the camera at
// `cam`. Returns the OpenGL rectangle texture of hit points (see rtm_trace_reflect), or 0.
unsigned rt_trace_reflect(uint64_t frame, const float *vp, const float *cam);
