// Ray-tracing scene capture: the game's 3D scene as triangles in world space, gathered from the
// draws of the scene pass (d3d9.cpp), for tinybvh acceleration structures.
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
    uint64_t mesh;               // identifies the geometry (buffers and range)
};

// Why a draw was left out of the capture (statistics).
enum RtSkip { RT_SKIP_SHADER, RT_SKIP_INSTANCED, RT_SKIP_UP, RT_SKIP_FORMAT, RT_SKIP_PRIM, RT_SKIP_NOCAM, RT_SKIP_N };

bool rt_capture_active(uint64_t frame);
void rt_capture(const RtDraw &d);
void rt_skip(RtSkip why, const char *what = nullptr);
// End of the frame: vp = camera view-projection (rows = clip components), ivp its inverse.
void rt_frame_end(uint64_t frame, const float *vp, const float *ivp);
