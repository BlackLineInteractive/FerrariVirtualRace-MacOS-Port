// Ray-tracing scene (see rt_scene.h).
//
// FVR_RT=1 (or rt=1 in options.txt) keeps the acceleration structures up to date every frame and,
// with FVR_RT_STATS=1, logs what they cost once a second. FVR_RT_DUMP=<seconds> traces one frame on
// the CPU through the same two levels (TLAS, then each instance's CWBVH) from the game's camera into
// rt_dump.ppm (FVR_SHOT_DIR or the current folder): when the picture matches the game's own frame,
// geometry, matrices and both levels are right.
#include "rt_scene.h"
#include "rt.h"
#include "rt_metal.h"
#include <SDL3/SDL.h>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <map>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <tuple>
#include <thread>
#include <atomic>
#include <unordered_set>

#define TINYBVH_IMPLEMENTATION
#include "../third_party/tiny_bvh.h"

using tinybvh::bvhvec3;
using tinybvh::bvhvec4;
using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---------------------------------------------------------------- BLAS cache
struct MeshKey {
    uint32_t vb, vbv, ib, ibv, stride, first, prims, uv, col;
    int32_t base;
    uint8_t prim, type, i32;
    bool operator==(const MeshKey &o) const {
        return vb == o.vb && vbv == o.vbv && ib == o.ib && ibv == o.ibv && stride == o.stride && first == o.first &&
               prims == o.prims && uv == o.uv && col == o.col && base == o.base && prim == o.prim && type == o.type && i32 == o.i32;
    }
};
struct MeshKeyHash {
    size_t operator()(const MeshKey &k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(k.vb); mix(k.vbv); mix(k.ib); mix(k.ibv); mix(k.stride); mix(k.first); mix(k.prims); mix(k.uv); mix(k.col);
        mix((uint32_t)k.base); mix(k.prim | k.type << 8 | k.i32 << 16);
        return (size_t)h;
    }
};
// the same draw range regardless of buffer versions: buffers rewritten every frame are not worth a BLAS
static MeshKey slot_of(MeshKey k) { k.vbv = k.ibv = 0; return k; }

// What a reflection hit needs to know about its triangle (as the Metal kernel reads it, 40 bytes):
// texture coordinates and baked light (COLOR1, RGBA8) of the three corners, and for the merged
// track the texture id (bits 0-30, 0x7FFFFFFF: the instance's) with bit 31 set when there is baked light.
struct TriAttr { float uv[6]; uint32_t c[3]; uint32_t tex; };
static const uint32_t kTexInst = 0x7FFFFFFFu, kHasBaked = 0x80000000u;

struct Blas {
    tinybvh::BVH8_CWBVH bvh;
    std::vector<bvhvec4> v;      // object-space positions
    std::vector<uint32_t> idx;   // 3 per triangle (padded to at least 4 triangles)
    uint32_t tris = 0;
    bvhvec3 lo{1e30f}, hi{-1e30f};
    uint64_t used = 0;           // last frame drawn
    uint32_t node_base = 0, tri_base = 0;   // in the packed GPU arrays (float4 units)
    std::vector<TriAttr> attr;   // per triangle of idx
    uint32_t attr_base = 0;
};
static std::unordered_map<MeshKey, std::unique_ptr<Blas>, MeshKeyHash> g_blas;
struct SlotInfo { uint32_t vbv, ibv; uint64_t changed; int rapid; bool known; };
static std::unordered_map<MeshKey, SlotInfo, MeshKeyHash> g_slots;

// ---------------------------------------------------------------- this frame's instances
struct Inst {
    Blas *b;
    float m[16];                 // as captured (object -> world 3 rows, or object -> clip)
    bool wvp;
    const char *shader;
    bool fixed;                  // world-space track geometry (kept in the world memory)
    int tex, kind;               // material (RtDraw)
    bool glass;
    float color[4], uvm[8];
    bool has_uvm;
    bool kept;                   // from the world memory, w / iw already set
    float w[12], iw[12];         // object -> world and world -> object (3x4 rows)
};
static std::vector<Inst> g_inst;
static uint64_t g_built = ~0ull;               // frame whose TLAS is built

// ---------------------------------------------------------------- world memory
// The game only draws what is in view, but reflections need what is behind the camera too. The
// track's world-space geometry (RtDraw::fixed: it cannot move) stays in the TLAS after it leaves
// the view, once it has been drawn in a few frames; cars and everything else come from this frame.
struct WKey {
    Blas *b; int32_t q[12];
    bool operator==(const WKey &o) const { return b == o.b && !memcmp(q, o.q, sizeof q); }
};
struct WKeyHash {
    size_t operator()(const WKey &k) const {
        uint64_t h = (uint64_t)(uintptr_t)k.b * 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < 12; i++) h = (h ^ (uint32_t)k.q[i]) * 1099511628211ull;
        return (size_t)h;
    }
};
struct WorldInst { float w[12], iw[12]; uint64_t seen; uint32_t frames; int tex; float uvm[8]; bool has_uvm; };
static std::unordered_map<WKey, WorldInst, WKeyHash> g_world;
static size_t g_world_kept;                    // persisted instances added to this frame's TLAS

// The remembered track merged into one world-space CWBVH (as the static group of the path tracer):
// overlapping track pieces as separate TLAS leaves make every ray test hundreds of them. Built in
// a background thread whenever enough new pieces are known; pieces in it leave the TLAS.
struct Merge {
    std::unique_ptr<Blas> blas;
    std::unordered_set<WKey, WKeyHash> keys;
    std::vector<bvhvec4> nodes, tris;      // packed for the GPU
    std::vector<TriAttr> attr;             // per triangle, texture coordinates already transformed
    double ms = 0;
};
static std::unique_ptr<Merge> g_merged;
static Merge *g_merge_job;                     // being built (owned by the worker until done)
static std::atomic<bool> g_merge_done{false};
static bool g_merged_new;                      // the GPU needs it
static uint64_t g_merge_started;
static size_t g_merge_pending;                 // remembered pieces not merged yet (stats)

static void merge_build(Merge *m) {
    auto t0 = Clock::now();
    Blas &b = *m->blas;
    b.tris = (uint32_t)(b.v.size() / 3);
    b.idx.resize(b.v.size());
    for (uint32_t i = 0; i < b.idx.size(); i++) b.idx[i] = i;
    b.bvh.Build(b.v.data(), b.idx.data(), b.tris);
    m->nodes.assign(b.bvh.bvh8Data, b.bvh.bvh8Data + b.bvh.usedBlocks);
    uint32_t tri_blocks = std::max(b.tris, b.bvh.bvh8.idxCount) * 4;
    m->tris.assign(b.bvh.bvh8Tris, b.bvh.bvh8Tris + tri_blocks);
    m->ms = ms_since(t0);
    g_merge_done = true;
}

static int world_frames() { static int n = -1; if (n < 0) { const char *e = getenv("FVR_RT_WORLD"); n = e ? atoi(e) : 10; } return n; }

// ---------------------------------------------------------------- packed for the GPU
// Every BLAS's CWBVH nodes and triangles, appended as they are built (the layout the Metal kernel
// walks); rebuilt from the cache after evictions. g_up_*: how much the GPU already has.
static std::vector<bvhvec4> g_pnodes, g_ptris;
static std::vector<TriAttr> g_pattr;
static size_t g_up_nodes, g_up_tris, g_up_attr;
static void pack(Blas &b) {
    b.node_base = (uint32_t)g_pnodes.size();
    b.tri_base = (uint32_t)g_ptris.size();
    g_pnodes.insert(g_pnodes.end(), b.bvh.bvh8Data, b.bvh.bvh8Data + b.bvh.usedBlocks);
    uint32_t tri_blocks = std::max((uint32_t)(b.idx.size() / 3), b.bvh.bvh8.idxCount) * 4;
    g_ptris.insert(g_ptris.end(), b.bvh.bvh8Tris, b.bvh.bvh8Tris + tri_blocks);
    b.attr_base = (uint32_t)g_pattr.size();
    g_pattr.insert(g_pattr.end(), b.attr.begin(), b.attr.end());
}
static tinybvh::BVH g_tlas;
static std::vector<bvhvec4> g_tlas_aabbs;

static std::map<std::string, uint64_t> g_skip_names;
static uint64_t g_dump_frame = ~0ull, g_frame = ~0ull;
static int g_dump_state;                       // 0 waiting, 1 this frame, 2 done
static uint64_t g_skips[RT_SKIP_N];
static uint64_t g_draws, g_new_blas, g_new_tris;
static double g_build_ms;

static double dump_time() {
    static double t = -2;
    if (t == -2) { const char *e = getenv("FVR_RT_DUMP"); t = e ? atof(e) : -1; }
    return t;
}
static bool stats_on() { static int on = -1; if (on < 0) on = getenv("FVR_RT_STATS") ? atoi(getenv("FVR_RT_STATS")) : 0; return on > 0; }

bool rt_capture_active(uint64_t frame, bool enabled) {
    if (g_dump_state == 0 && dump_time() >= 0 && SDL_GetTicks() / 1000.0 >= dump_time()) { g_dump_state = 1; g_dump_frame = frame; }
    bool on = enabled || (g_dump_state == 1 && frame == g_dump_frame);
    if (on && frame != g_frame) { g_frame = frame; g_inst.clear(); }
    return on;
}

void rt_skip(RtSkip why, const char *what) {
    g_skips[why]++;
    if (what && g_dump_state == 1) g_skip_names[what]++;
}

// Gathers the draw's triangles in object space. False when nothing usable is left.
static bool fill_blas(const RtDraw &d, Blas &b) {
    uint32_t nidx = d.prim == 4 ? d.prims * 3 : d.prims + 2;
    auto index = [&](uint32_t k) -> int64_t {
        if (!d.ib) return (int64_t)d.first + k;
        return (int64_t)(d.i32 ? ((const uint32_t *)d.ib)[d.first + k] : ((const uint16_t *)d.ib)[d.first + k]) + d.base_vertex;
    };
    // vertices actually referenced, copied once each
    std::unordered_map<int64_t, uint32_t> remap;
    remap.reserve(nidx);
    std::vector<float> vuv;
    std::vector<uint32_t> vcol;
    auto vertex = [&](int64_t v, uint32_t &out) -> bool {
        auto it = remap.find(v);
        if (it != remap.end()) { out = it->second; return true; }
        if (v < 0 || (uint64_t)v * d.stride + 12 > d.vb_size) return false;
        float p[3]; memcpy(p, d.pos + v * d.stride, 12);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
        out = (uint32_t)b.v.size();
        b.v.push_back(bvhvec4(p[0], p[1], p[2], 0));
        float uv[2] = {0, 0};
        if (d.uv && (uint64_t)v * d.uv_stride + 8 <= d.uv_size) memcpy(uv, d.uv + v * d.uv_stride, 8);
        if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) uv[0] = uv[1] = 0;
        vuv.push_back(uv[0]); vuv.push_back(uv[1]);
        uint32_t c = 0xFFFFFFFFu;
        if (d.col && (uint64_t)v * d.col_stride + 4 <= d.col_size) {
            uint32_t argb; memcpy(&argb, d.col + v * d.col_stride, 4);   // D3DCOLOR -> RGBA8
            c = ((argb >> 16) & 255) | (argb & 0xFF00) | ((argb & 255) << 16) | (argb & 0xFF000000u);
        }
        vcol.push_back(c);
        b.lo = tinybvh::tinybvh_min(b.lo, bvhvec3(p[0], p[1], p[2]));
        b.hi = tinybvh::tinybvh_max(b.hi, bvhvec3(p[0], p[1], p[2]));
        remap.emplace(v, out);
        return true;
    };
    for (uint32_t t = 0; t < d.prims; t++) {
        uint32_t i0, i1, i2;
        if (d.prim == 4) { i0 = t * 3; i1 = i0 + 1; i2 = i0 + 2; }
        else { i0 = t; i1 = t + 1 + (t & 1); i2 = t + 2 - (t & 1); }
        if (i2 >= nidx) break;
        int64_t a = index(i0), c = index(i1), e = index(i2);
        if (a == c || c == e || a == e) continue;           // strip joins
        uint32_t ra, rc, re;
        if (!vertex(a, ra) || !vertex(c, rc) || !vertex(e, re)) continue;
        b.idx.push_back(ra); b.idx.push_back(rc); b.idx.push_back(re);
    }
    b.tris = (uint32_t)(b.idx.size() / 3);
    if (!b.tris) return false;
    // tinybvh needs a few triangles to build a wide BVH: repeat small meshes
    size_t orig = b.idx.size();
    while (b.idx.size() / 3 < 4) b.idx.insert(b.idx.end(), b.idx.begin(), b.idx.begin() + orig);
    b.attr.resize(b.idx.size() / 3);
    for (size_t t = 0; t < b.attr.size(); t++) {
        TriAttr &a = b.attr[t];
        for (int k = 0; k < 3; k++) {
            uint32_t vi = b.idx[t * 3 + k];
            a.uv[k * 2] = vuv[vi * 2]; a.uv[k * 2 + 1] = vuv[vi * 2 + 1];
            a.c[k] = vcol[vi];
        }
        a.tex = kTexInst | (d.col ? kHasBaked : 0);
    }
    b.bvh.Build(b.v.data(), b.idx.data(), (uint32_t)(b.idx.size() / 3));
    return true;
}

static double build_budget_ms() {
    static double ms = -1;
    if (ms < 0) { const char *e = getenv("FVR_RT_BUDGET"); ms = e ? atof(e) : 6.0; }
    return ms;
}

void rt_capture(const RtDraw &d) {
    if (d.pos_type != 2 && d.pos_type != 3) { g_skips[RT_SKIP_FORMAT]++; return; }
    if (d.prim != 4 && d.prim != 5) { g_skips[RT_SKIP_PRIM]++; return; }
    MeshKey k{d.vb_id, d.vb_ver, d.ib_id, d.ib_ver, d.stride, d.first, d.prims, d.uv ? d.uv_id : 0, d.col ? d.col_id : 0, d.base_vertex,
              (uint8_t)d.prim, d.pos_type, (uint8_t)d.i32};
    Blas *b = nullptr;
    auto it = g_blas.find(k);
    if (it != g_blas.end()) b = it->second.get();
    else {
        SlotInfo &s = g_slots[slot_of(k)];
        // contents rewritten: occasionally (car damage) gets a new BLAS, every few frames (particles,
        // debris) is left out until it settles
        if (!s.known) { s.known = true; s.changed = g_frame; s.rapid = 0; }
        else if (s.vbv != k.vbv || s.ibv != k.ibv) { s.rapid = g_frame - s.changed <= 8 ? s.rapid + 1 : 0; s.changed = g_frame; }
        s.vbv = k.vbv; s.ibv = k.ibv;
        if (s.rapid >= 3 && g_frame - s.changed <= 8) { g_skips[RT_SKIP_VOLATILE]++; return; }
        bool dump = g_dump_state == 1 && g_frame == g_dump_frame;
        if (!dump && g_build_ms > build_budget_ms()) { g_skips[RT_SKIP_BUDGET]++; return; }   // next frame
        auto t0 = Clock::now();
        auto nb = std::make_unique<Blas>();
        bool ok = fill_blas(d, *nb);
        g_build_ms += ms_since(t0);
        if (!ok) { g_skips[RT_SKIP_EMPTY]++; return; }
        g_new_blas++; g_new_tris += nb->tris;
        pack(*nb);
        b = nb.get();
        g_blas.emplace(k, std::move(nb));
    }
    b->used = g_frame;
    Inst in;
    in.b = b; memcpy(in.m, d.m, 64); in.wvp = d.wvp; in.shader = d.shader; in.fixed = d.fixed; in.kept = false;
    in.tex = d.tex; in.kind = d.kind; in.glass = d.glass; memcpy(in.color, d.color, 16); memcpy(in.uvm, d.uvm, 32); in.has_uvm = d.has_uvm;
    g_inst.push_back(in);
    g_draws++;
}

// ---------------------------------------------------------------- end of frame: instances and TLAS
static void mul4(const float *a, const float *b, float *r) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = v; }
}
static bool inv34(const float *m, float *r) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-20f) return false;
    float id = 1.0f / det;
    r[0] = A * id; r[1] = (c * h - b * i) * id; r[2] = (b * f - c * e) * id;
    r[4] = B * id; r[5] = (a * i - c * g) * id; r[6] = (c * d - a * f) * id;
    r[8] = C * id; r[9] = (b * g - a * h) * id; r[10] = (a * e - b * d) * id;
    for (int row = 0; row < 3; row++) r[row * 4 + 3] = -(r[row * 4] * m[3] + r[row * 4 + 1] * m[7] + r[row * 4 + 2] * m[11]);
    return true;
}
static inline bvhvec3 xp(const float *w, const bvhvec3 &p) {
    return bvhvec3(w[0] * p.x + w[1] * p.y + w[2] * p.z + w[3], w[4] * p.x + w[5] * p.y + w[6] * p.z + w[7], w[8] * p.x + w[9] * p.y + w[10] * p.z + w[11]);
}
static inline bvhvec3 xd(const float *w, const bvhvec3 &v) {
    return bvhvec3(w[0] * v.x + w[1] * v.y + w[2] * v.z, w[4] * v.x + w[5] * v.y + w[6] * v.z, w[8] * v.x + w[9] * v.y + w[10] * v.z);
}

// Object -> world for every instance, then the TLAS over their world bounds.
// Inverse of a 4x4 (rows) in double precision; false if singular.
static bool inv4d(const float *m, double *r) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int i = c + 1; i < 4; i++) if (fabs(a[i][c]) > fabs(a[p][c])) p = i;
        if (fabs(a[p][c]) < 1e-30) return false;
        if (p != c) for (int j = 0; j < 8; j++) std::swap(a[p][j], a[c][j]);
        double iv = 1.0 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= iv;
        for (int i = 0; i < 4; i++) if (i != c) { double f = a[i][c]; for (int j = 0; j < 8; j++) a[i][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i][j + 4];
    return true;
}

static bool build_tlas(const float *vp) {
    double ivp[16];
    if (!inv4d(vp, ivp)) return false;
    g_tlas_aabbs.clear();
    size_t n = 0;
    const uint64_t frame = g_frame;
    for (size_t i = 0; i < g_inst.size(); i++) {
        Inst &in = g_inst[i];
        if (in.wvp) {   // world = inverse(camera) * WorldViewProj; the bottom row is (0 0 0 1) up to rounding
            double w[16];
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
                double v = 0; for (int k = 0; k < 4; k++) v += ivp[r * 4 + k] * in.m[k * 4 + c]; w[r * 4 + c] = v;
            }
            double s = fabs(w[15]) > 1e-12 ? 1.0 / w[15] : 1.0;
            for (int k = 0; k < 12; k++) in.w[k] = (float)(w[k] * s);
        } else memcpy(in.w, in.m, 48);
        if (!inv34(in.w, in.iw)) continue;
        g_inst[n++] = in;
    }
    g_inst.resize(n);
    for (size_t i = 0; i < n; i++) {
        Inst &in = g_inst[i];
        Blas *b = in.b;
        if (in.fixed) {
            WKey k; k.b = b;
            for (int j = 0; j < 12; j++) k.q[j] = (int32_t)lrintf(in.w[j] * ((j & 3) == 3 ? 10.0f : 100.0f));
            if (g_merged && g_merged->keys.count(k)) in.kept = true;   // already in the merged track: dropped below
            auto it = g_world.find(k);
            if (it == g_world.end()) {
                WorldInst wi; memcpy(wi.w, in.w, 48); memcpy(wi.iw, in.iw, 48); wi.seen = frame; wi.frames = 1;
                wi.tex = in.tex; memcpy(wi.uvm, in.uvm, 32); wi.has_uvm = in.has_uvm;
                g_world.emplace(k, wi);
            } else if (it->second.seen != frame) { it->second.seen = frame; it->second.frames++; }
        }
    }
    // static placements not drawn this frame
    // the merged track: take a finished build, drop the pieces it holds
    if (g_merge_done) {
        g_merge_done = false;
        g_merged.reset(g_merge_job);
        g_merge_job = nullptr;
        g_merged_new = true;
        if (stats_on()) rtlog("rt: merged track rebuilt: %zu pieces, %u triangles, %.0f ms in the background", g_merged->keys.size(), g_merged->blas->tris, g_merged->ms);
    }
    {
        size_t m = 0;
        for (size_t i = 0; i < n; i++) if (!(g_inst[i].fixed && g_inst[i].kept)) g_inst[m++] = g_inst[i];
        g_inst.resize(m);
    }
    g_world_kept = 0;
    g_merge_pending = 0;
    size_t established = 0;
    for (auto it = g_world.begin(); it != g_world.end();) {
        WorldInst &w = it->second;
        Blas *b = it->first.b;
        if ((w.frames < (uint32_t)world_frames() && frame - w.seen > 2)) { it = g_world.erase(it); continue; }
        bool merged = g_merged && g_merged->keys.count(it->first);
        if (w.frames >= (uint32_t)world_frames()) { established++; if (!merged) g_merge_pending++; it->first.b->used = frame; }
        if (w.seen != frame && w.frames >= (uint32_t)world_frames() && !merged) {
            Inst in; in.b = b; in.wvp = false; in.shader = "world memory"; in.fixed = true; in.kept = true;
            in.tex = w.tex; in.kind = 0; in.glass = false; in.color[0] = in.color[1] = in.color[2] = in.color[3] = 1; memcpy(in.uvm, w.uvm, 32); in.has_uvm = w.has_uvm;
            memcpy(in.w, w.w, 48); memcpy(in.iw, w.iw, 48);
            b->used = frame;
            g_inst.push_back(in);
            g_world_kept++;
        }
        ++it;
    }
    if (g_merged) {
        Inst in; in.b = g_merged->blas.get(); in.wvp = false; in.shader = "merged track"; in.fixed = false; in.kept = true;
        in.tex = -1; in.kind = 0; in.glass = false; in.color[0] = in.color[1] = in.color[2] = in.color[3] = 1; in.has_uvm = false;
        static const float I[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        memcpy(in.w, I, 48); memcpy(in.iw, I, 48);
        g_inst.push_back(in);
    }
    // start a new merge when enough new pieces are known (at most once a second)
    if (!g_merge_job && g_merge_pending >= 32 && SDL_GetTicks() - g_merge_started >= 1000) {
        g_merge_started = SDL_GetTicks();
        Merge *m = new Merge;
        m->blas = std::make_unique<Blas>();
        Blas &mb = *m->blas;
        for (auto &e : g_world) {
            if (e.second.frames < (uint32_t)world_frames()) continue;
            m->keys.insert(e.first);
            const Blas &b = *e.first.b;
            for (uint32_t t = 0; t < b.tris * 3; t++) {
                bvhvec3 p = xp(e.second.w, bvhvec3(b.v[b.idx[t]]));
                mb.v.push_back(bvhvec4(p.x, p.y, p.z, 0));
                mb.lo = tinybvh::tinybvh_min(mb.lo, p); mb.hi = tinybvh::tinybvh_max(mb.hi, p);
            }
            const WorldInst &wi = e.second;
            for (uint32_t t = 0; t < b.tris; t++) {
                TriAttr a = b.attr[t];
                if (wi.has_uvm)
                    for (int k = 0; k < 3; k++) {
                        float u = a.uv[k * 2], v = a.uv[k * 2 + 1];
                        a.uv[k * 2] = wi.uvm[0] * u + wi.uvm[1] * v + wi.uvm[2];
                        a.uv[k * 2 + 1] = wi.uvm[4] * u + wi.uvm[5] * v + wi.uvm[6];
                    }
                a.tex = (a.tex & kHasBaked) | (wi.tex >= 0 ? (uint32_t)wi.tex : kTexInst);
                m->attr.push_back(a);
            }
        }
        if (mb.v.size() >= 12) { g_merge_job = m; std::thread(merge_build, m).detach(); }
        else delete m;
    }
    n = g_inst.size();
    for (size_t i = 0; i < n; i++) {
        const Inst &in = g_inst[i];
        bvhvec3 lo(1e30f), hi(-1e30f);
        for (int c = 0; c < 8; c++) {
            bvhvec3 p((c & 1) ? in.b->hi.x : in.b->lo.x, (c & 2) ? in.b->hi.y : in.b->lo.y, (c & 4) ? in.b->hi.z : in.b->lo.z);
            p = xp(in.w, p);
            lo = tinybvh::tinybvh_min(lo, p); hi = tinybvh::tinybvh_max(hi, p);
        }
        g_tlas_aabbs.push_back(bvhvec4(lo.x, lo.y, lo.z, 0));
        g_tlas_aabbs.push_back(bvhvec4(hi.x, hi.y, hi.z, 0));
    }
    if (n < 2) return false;
    g_tlas.BuildAABB(g_tlas_aabbs.data(), (uint32_t)n);
    return true;
}

// Frees BLASes that have not been drawn for a while (meshes of other places on the track stay).
static void evict(uint64_t frame) {
    if (frame % 600) return;
    size_t before = g_blas.size();
    for (auto it = g_blas.begin(); it != g_blas.end();)
        if (frame - it->second->used > 3600) it = g_blas.erase(it); else ++it;
    if (g_blas.size() != before) {   // repack what is left
        g_pnodes.clear(); g_ptris.clear(); g_pattr.clear(); g_up_nodes = g_up_tris = g_up_attr = 0;
        for (auto &k : g_blas) pack(*k.second);
    }
    if (g_slots.size() > 200000) g_slots.clear();
}

// ---------------------------------------------------------------- CPU check: trace both levels
struct Hit { float t = 1e30f; uint32_t inst = ~0u, prim = 0; };

static void trace(const bvhvec3 &O, const bvhvec3 &D, Hit &hit) {
    const tinybvh::BVH::BVHNode *nodes = g_tlas.bvhNode;
    uint32_t stack[64], sp = 0, node = 0;
    bvhvec3 rD(1.0f / D.x, 1.0f / D.y, 1.0f / D.z);
    auto box = [&](const tinybvh::BVH::BVHNode &nd) -> float {
        float tx1 = (nd.aabbMin.x - O.x) * rD.x, tx2 = (nd.aabbMax.x - O.x) * rD.x;
        float ty1 = (nd.aabbMin.y - O.y) * rD.y, ty2 = (nd.aabbMax.y - O.y) * rD.y;
        float tz1 = (nd.aabbMin.z - O.z) * rD.z, tz2 = (nd.aabbMax.z - O.z) * rD.z;
        float tmin = std::max(std::max(std::min(tx1, tx2), std::min(ty1, ty2)), std::max(std::min(tz1, tz2), 0.0f));
        float tmax = std::min(std::min(std::max(tx1, tx2), std::max(ty1, ty2)), std::min(std::max(tz1, tz2), hit.t));
        return tmin <= tmax ? tmin : 1e30f;
    };
    while (true) {
        const tinybvh::BVH::BVHNode &nd = nodes[node];
        if (nd.isLeaf()) {
            for (uint32_t i = 0; i < nd.triCount; i++) {
                uint32_t ii = g_tlas.primIdx[nd.leftFirst + i];
                const Inst &in = g_inst[ii];
                // object-space ray; tinybvh normalises the direction, so scale t back
                bvhvec3 o = xp(in.iw, O), dv = xd(in.iw, D);
                float s = sqrtf(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
                if (s < 1e-20f) continue;
                tinybvh::Ray r(o, dv, hit.t * s);
                in.b->bvh.Intersect(r);
                if (r.hit.t < hit.t * s) { hit.t = r.hit.t / s; hit.inst = ii; hit.prim = r.hit.prim; }
            }
            if (!sp) break;
            node = stack[--sp];
            continue;
        }
        uint32_t c1 = nd.leftFirst, c2 = nd.leftFirst + 1;
        float d1 = box(nodes[c1]), d2 = box(nodes[c2]);
        if (d1 > d2) { std::swap(d1, d2); std::swap(c1, c2); }
        if (d1 == 1e30f) { if (!sp) break; node = stack[--sp]; continue; }
        node = c1;
        if (d2 != 1e30f && sp < 64) stack[sp++] = c2;
    }
}

static void dump(const float *ivp) {
    std::vector<std::pair<uint64_t, std::string>> top;
    for (auto &k : g_skip_names) top.push_back({k.second, k.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 12; i++) rtlog("rt: skipped %llu draws of shader [%s]", (unsigned long long)top[i].first, top[i].second.c_str());
    size_t tris = 0;
    for (auto &in : g_inst) tris += in.b->tris;
    rtlog("rt: %zu instances (%zu triangles), %zu BLAS in the cache; skipped: shader %llu, instanced %llu, UP %llu, format %llu, prim %llu, volatile %llu, empty %llu",
          g_inst.size(), tris, g_blas.size(), (unsigned long long)g_skips[RT_SKIP_SHADER], (unsigned long long)g_skips[RT_SKIP_INSTANCED],
          (unsigned long long)g_skips[RT_SKIP_UP], (unsigned long long)g_skips[RT_SKIP_FORMAT], (unsigned long long)g_skips[RT_SKIP_PRIM],
          (unsigned long long)g_skips[RT_SKIP_VOLATILE], (unsigned long long)g_skips[RT_SKIP_EMPTY]);
    {   // instance bounds: how many are huge or identical (they defeat the TLAS)
        int hist[12] = {};
        std::map<std::tuple<float, float, float, float, float, float>, int> same;
        for (size_t i = 0; i < g_inst.size(); i++) {
            const bvhvec4 &lo = g_tlas_aabbs[i * 2], &hi = g_tlas_aabbs[i * 2 + 1];
            float d = sqrtf((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) + (hi.z - lo.z) * (hi.z - lo.z));
            int b = d < 1 ? 0 : std::min(11, (int)log10f(d) * 2 + 1);
            hist[b]++;
            same[{lo.x, lo.y, lo.z, hi.x, hi.y, hi.z}]++;
        }
        rtlog("rt: instance size histogram (diagonal <1, 1-3, 3-10, 10-30, ...): %d %d %d %d %d %d %d %d %d %d", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], hist[8], hist[9]);
        int dup = 0, top = 0; std::tuple<float, float, float, float, float, float> tk;
        for (auto &k : same) { if (k.second > 1) dup += k.second; if (k.second > top) { top = k.second; tk = k.first; } }
        std::map<Blas *, int> blases; const Inst *ex = nullptr;
        for (size_t i = 0; i < g_inst.size(); i++) {
            const bvhvec4 &lo = g_tlas_aabbs[i * 2], &hi = g_tlas_aabbs[i * 2 + 1];
            if (std::make_tuple(lo.x, lo.y, lo.z, hi.x, hi.y, hi.z) == tk) { blases[g_inst[i].b]++; ex = &g_inst[i]; }
        }
        std::map<std::string, int> sh;
        for (size_t i = 0; i < g_inst.size(); i++) {
            const bvhvec4 &lo = g_tlas_aabbs[i * 2], &hi = g_tlas_aabbs[i * 2 + 1];
            if (std::make_tuple(lo.x, lo.y, lo.z, hi.x, hi.y, hi.z) == tk) sh[g_inst[i].shader ? g_inst[i].shader : "?"]++;
        }
        for (auto &k : sh) rtlog("rt: largest group shader [%s]: %d", k.first.c_str(), k.second);
        if (ex) rtlog("rt: largest group: %zu distinct BLAS, e.g. %u triangles, wvp %d, bounds %.1f %.1f %.1f .. %.1f %.1f %.1f, object bounds %.1f %.1f %.1f .. %.1f %.1f %.1f",
                      blases.size(), ex->b->tris, ex->wvp, std::get<0>(tk), std::get<1>(tk), std::get<2>(tk), std::get<3>(tk), std::get<4>(tk), std::get<5>(tk),
                      ex->b->lo.x, ex->b->lo.y, ex->b->lo.z, ex->b->hi.x, ex->b->hi.y, ex->b->hi.z);
        rtlog("rt: %d instances share their bounds with another one (largest group %d)", dup, top);
        rtlog("rt: TLAS %u nodes", g_tlas.usedNodes);
        std::map<std::string, std::pair<int, size_t>> by;
        for (auto &in : g_inst) { auto &e = by[in.shader ? in.shader : "?"]; e.first++; e.second += in.b->tris; }
        for (auto &k : by) rtlog("rt: shader [%s]: %d instances, %zu triangles", k.first.c_str(), k.second.first, k.second.second);
    }
    const int W = 960, H = 540;
    std::vector<uint8_t> img(W * H * 3);
    auto unproj = [&](float x, float y, float z) {
        float v[4];
        for (int i = 0; i < 4; i++) v[i] = ivp[i * 4] * x + ivp[i * 4 + 1] * y + ivp[i * 4 + 2] * z + ivp[i * 4 + 3];
        return bvhvec3(v[0] / v[3], v[1] / v[3], v[2] / v[3]);
    };
    auto t0 = Clock::now();
    for (int py = 0; py < H; py++)
        for (int px = 0; px < W; px++) {
            float x = (px + 0.5f) / W * 2 - 1, y = 1 - (py + 0.5f) / H * 2;
            bvhvec3 a = unproj(x, y, 0.0f), b = unproj(x, y, 1.0f);
            bvhvec3 Dv(b.x - a.x, b.y - a.y, b.z - a.z);
            float len = sqrtf(Dv.x * Dv.x + Dv.y * Dv.y + Dv.z * Dv.z);
            Dv = Dv * (1.0f / len);
            Hit h; h.t = len;
            trace(a, Dv, h);
            uint8_t *o = &img[(py * W + px) * 3];
            if (h.inst == ~0u) { o[0] = 90; o[1] = 120; o[2] = 160; continue; }   // sky
            const Inst &in = g_inst[h.inst];
            const uint32_t *ix = &in.b->idx[h.prim * 3];
            bvhvec3 v0 = xp(in.w, bvhvec3(in.b->v[ix[0]])), v1 = xp(in.w, bvhvec3(in.b->v[ix[1]])), v2 = xp(in.w, bvhvec3(in.b->v[ix[2]]));
            bvhvec3 nn = tinybvh::tinybvh_cross(v1 - v0, v2 - v0);
            float nl = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z) + 1e-20f;
            float sh = fabsf((nn.x * Dv.x + nn.y * Dv.y + nn.z * Dv.z) / nl);
            float up = fabsf(nn.y / nl);
            float fog = expf(-h.t * 0.0015f);
            // a colour per instance, so the instance boundaries show
            uint32_t hc = h.inst * 2654435761u;
            float tint[3] = {0.75f + 0.25f * ((hc >> 8) & 255) / 255.0f, 0.75f + 0.25f * ((hc >> 16) & 255) / 255.0f, 0.75f + 0.25f * ((hc >> 24) & 255) / 255.0f};
            o[0] = (uint8_t)(255 * fog * tint[0] * (0.15f + 0.6f * sh));
            o[1] = (uint8_t)(255 * fog * tint[1] * (0.15f + 0.6f * sh * (0.6f + 0.4f * up)));
            o[2] = (uint8_t)(255 * fog * tint[2] * (0.15f + 0.45f * sh));
        }
    rtlog("rt: traced %dx%d primary rays through TLAS + CWBVH on the CPU in %.0f ms", W, H, ms_since(t0));
    const char *dir = getenv("FVR_SHOT_DIR");
    std::string path = std::string(dir ? dir : ".") + "/rt_dump.ppm";
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        fwrite(img.data(), 1, img.size(), f);
        fclose(f);
        rtlog("rt: saved %s", path.c_str());
    }
}

void rt_frame_end(uint64_t frame, const float *vp, const float *ivp) {
    if (frame != g_frame) return;
    auto t0 = Clock::now();
    bool ok = g_built == frame ? g_inst.size() >= 2 : build_tlas(vp);
    g_built = frame;
    double tlas_ms = ms_since(t0);
    static uint64_t last_log;
    if (stats_on() && SDL_GetTicks() - last_log >= 1000) {
        last_log = SDL_GetTicks();
        size_t tris = 0;
        for (auto &in : g_inst) tris += in.b->tris;
        rtlog("rt: %zu instances, %zu triangles, %zu BLAS (+%llu, +%llu triangles, %.1f ms), TLAS %.2f ms; skipped volatile %llu, budget %llu",
              g_inst.size(), tris, g_blas.size(), (unsigned long long)g_new_blas, (unsigned long long)g_new_tris, g_build_ms, tlas_ms,
              (unsigned long long)g_skips[RT_SKIP_VOLATILE], (unsigned long long)g_skips[RT_SKIP_BUDGET]);
        g_new_blas = g_new_tris = 0;
    }
    if (g_dump_state == 1 && frame == g_dump_frame) {
        g_dump_state = 2;
        if (ok) dump(ivp);
    }
    g_build_ms = 0;
    evict(frame);
}

// The GPU view (FVR_RT_VIEW): this frame's TLAS, built now (the scene pass is done), and one primary
// ray per pixel traced by Metal into an image OpenGL can read.
// ---------------------------------------------------------------- textures for hit shading
// Every diffuse texture a captured draw uses, decoded once to RGBA8 (r | g << 8 | b << 16 | a << 24)
// at most 128 pixels on a side, in one buffer, as the kernel samples them (bilinear, repeating).
struct TexInfo { uint32_t off, w, h, pad; };
static std::vector<uint32_t> g_texels;
static std::vector<TexInfo> g_texinfo;
static std::unordered_map<uint64_t, int> g_texids;
static size_t g_up_texels, g_up_texinfo;

static inline uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }
static void dxt_block(uint32_t fmt, const uint8_t *b, uint32_t out[16]) {
    const bool dxt1 = fmt == 0x31545844u;
    const uint8_t *cb = dxt1 ? b : b + 8;
    uint16_t c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8;
    int p[4][4];
    auto u565 = [](uint16_t c, int *o) { o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; o[3] = 255; };
    u565(c0, p[0]); u565(c1, p[1]);
    if (!dxt1 || c0 > c1) for (int k = 0; k < 3; k++) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
    else for (int k = 0; k < 3; k++) { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
    p[2][3] = 255; p[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
    uint32_t bits = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
    int alpha[16];
    for (int i = 0; i < 16; i++) alpha[i] = -1;
    if (fmt == 0x33545844u) for (int i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> ((i & 1) * 4)) & 15) * 17;
    if (fmt == 0x35545844u) {
        int a0 = b[0], a1 = b[1], ap[8] = {a0, a1};
        if (a0 > a1) for (int k = 1; k < 7; k++) ap[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        else { for (int k = 1; k < 5; k++) ap[k + 1] = ((5 - k) * a0 + k * a1) / 5; ap[6] = 0; ap[7] = 255; }
        uint64_t ab = 0;
        for (int k = 0; k < 6; k++) ab |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) alpha[i] = ap[(ab >> (3 * i)) & 7];
    }
    for (int i = 0; i < 16; i++) {
        const int *c = p[(bits >> (2 * i)) & 3];
        out[i] = rgba(c[0], c[1], c[2], alpha[i] >= 0 ? alpha[i] : c[3]);
    }
}

int rt_texture(const RtTexSrc &t) {
    auto it = g_texids.find(t.key);
    if (it != g_texids.end()) return it->second;
    const bool dxt = t.fmt == 0x31545844u || t.fmt == 0x33545844u || t.fmt == 0x35545844u;
    if (!t.data || !t.w || !t.h || !(dxt || t.fmt == 21 || t.fmt == 22)) { g_texids[t.key] = -1; return -1; }
    std::vector<uint32_t> full((size_t)t.w * t.h);
    if (dxt) {
        uint32_t blk[16];
        for (uint32_t by = 0; by < (t.h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (t.w + 3) / 4; bx++) {
                dxt_block(t.fmt, t.data + by * t.pitch + bx * (t.fmt == 0x31545844u ? 8 : 16), blk);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                    if (x < t.w && y < t.h) full[(size_t)y * t.w + x] = blk[i];
                }
            }
    } else {
        for (uint32_t y = 0; y < t.h; y++)
            for (uint32_t x = 0; x < t.w; x++) {
                const uint8_t *q = t.data + y * t.pitch + x * 4;   // B G R A
                full[(size_t)y * t.w + x] = rgba(q[2], q[1], q[0], t.fmt == 22 ? 255 : q[3]);
            }
    }
    // box filter down to at most 128 x 128
    uint32_t f = 1;
    while (std::max(t.w, t.h) / f > 128) f *= 2;
    uint32_t ow = std::max(1u, t.w / f), oh = std::max(1u, t.h / f);
    TexInfo ti{(uint32_t)g_texels.size(), ow, oh, 0};
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t acc[4] = {}, n = 0;
            for (uint32_t j = 0; j < f && y * f + j < t.h; j++)
                for (uint32_t i = 0; i < f && x * f + i < t.w; i++) {
                    uint32_t c = full[(size_t)(y * f + j) * t.w + x * f + i];
                    for (int k = 0; k < 4; k++) acc[k] += (c >> (8 * k)) & 255;
                    n++;
                }
            g_texels.push_back(rgba(acc[0] / n, acc[1] / n, acc[2] / n, acc[3] / n));
        }
    int id = (int)g_texinfo.size();
    g_texinfo.push_back(ti);
    g_texids[t.key] = id;
    return id;
}

static RtmLight g_light = {{0, 1, 0, 0}, {1, 1, 1, 0}, {0.2f, 0.2f, 0.2f, 0}, {0.4f, 0.4f, 0.5f, 0}, 1.0f, {0, 0, 0}};
void rt_lighting(const float *sun_dir, const float *sun_col, const float *amb_lo, const float *amb_hi, float amb_level) {
    if (sun_dir) memcpy(g_light.sun_dir, sun_dir, 12);
    if (sun_col) memcpy(g_light.sun_col, sun_col, 12);
    if (amb_lo) memcpy(g_light.amb_lo, amb_lo, 12);
    if (amb_hi) memcpy(g_light.amb_hi, amb_hi, 12);
    if (amb_level > 0) g_light.amb_level = amb_level;
}

// This frame's TLAS (built now: the scene pass is done) and instances, BLAS data uploaded.
static std::vector<RtmInst> g_gi;
static double g_tlas_ms;
static bool gpu_frame(uint64_t frame, const float *vp) {
    if (frame != g_frame || !rtm_available()) return false;
    auto t0 = Clock::now();
    if (g_built != frame) { g_built = frame; if (!build_tlas(vp)) g_inst.clear(); }
    g_tlas_ms = ms_since(t0);
    if (g_inst.empty() || g_pnodes.empty()) return false;
    rtm_blas(g_pnodes.data(), g_pnodes.size(), g_up_nodes, g_ptris.data(), g_ptris.size(), g_up_tris);
    g_up_nodes = g_pnodes.size(); g_up_tris = g_ptris.size();
    rtm_attrs(g_pattr.data(), g_pattr.size(), g_up_attr);
    g_up_attr = g_pattr.size();
    rtm_textures(g_texels.data(), g_texels.size(), g_up_texels, g_texinfo.data(), g_texinfo.size());
    g_up_texels = g_texels.size(); g_up_texinfo = g_texinfo.size();
    if (g_merged_new && g_merged) {
        rtm_merged(g_merged->nodes.data(), g_merged->nodes.size(), g_merged->tris.data(), g_merged->tris.size(), g_merged->attr.data(), g_merged->attr.size());
        g_merged_new = false;
    }
    g_gi.resize(g_inst.size());
    for (size_t i = 0; i < g_inst.size(); i++) {
        memcpy(g_gi[i].w2o, g_inst[i].iw, 48);
        bool merged = g_merged && g_inst[i].b == g_merged->blas.get();
        g_gi[i].node_base = merged ? 0 : g_inst[i].b->node_base;
        g_gi[i].tri_base = merged ? 0 : g_inst[i].b->tri_base;
        g_gi[i].attr_base = merged ? 0 : g_inst[i].b->attr_base;
        const Inst &in = g_inst[i];
        g_gi[i].flags = (merged ? 1u : 0u) | (in.kind == 1 ? 2u : 0u) | (in.has_uvm ? 4u : 0u) | (in.glass ? 8u : 0u);
        memcpy(g_gi[i].color, in.color, 16);
        memcpy(g_gi[i].uvm, in.uvm, 32);
        g_gi[i].tex = in.tex; g_gi[i].pad[0] = g_gi[i].pad[1] = g_gi[i].pad[2] = 0;
    }
    return true;
}

unsigned rt_trace_view(uint64_t frame, const float *vp, const float *ivp, int w, int h, int mode) {
    auto t0 = Clock::now();
    if (!gpu_frame(frame, vp)) return 0;
    double gpu = 0;
    unsigned tex = rtm_trace_view(g_gi.data(), g_gi.size(), g_tlas.bvhNode, g_tlas.usedNodes, g_tlas.primIdx, g_inst.size(), ivp, w, h, mode, &gpu);
    double all = ms_since(t0);
    static uint64_t last;
    if (stats_on() && SDL_GetTicks() - last >= 1000) {
        last = SDL_GetTicks();
        rtlog("rt: GPU view %dx%d: Metal %.2f ms (wall %.2f ms incl. TLAS and upload), TLAS %.2f ms, %zu instances (%zu remembered, %zu waiting for the merge, merged track %u triangles), BLAS data %.1f MB",
              w, h, gpu, all, g_tlas_ms, g_inst.size(), g_world_kept, g_merge_pending, g_merged ? g_merged->blas->tris : 0,
              (g_pnodes.size() + g_ptris.size()) * 16 / 1048576.0);
    }
    return tex;
}

unsigned rt_trace_reflect(uint64_t frame, const float *vp, const float *cam, bool refract) {
    auto t0 = Clock::now();
    if (!gpu_frame(frame, vp)) return 0;
    double gpu = 0;
    uint32_t rays[4] = {};
    unsigned tex = rtm_trace_reflect(g_gi.data(), g_gi.size(), g_tlas.bvhNode, g_tlas.usedNodes, g_tlas.primIdx, g_inst.size(), cam, g_light, refract, rays, &gpu);
    double all = ms_since(t0);
    static uint64_t last;
    if (stats_on() && SDL_GetTicks() - last >= 1000) {
        last = SDL_GetTicks();
        rtlog("rt: reflections: %u rays (glass %u, of which %u through a second surface, mean thickness %.3f), Metal %.2f ms (wall %.2f ms incl. TLAS %.2f ms and upload), %zu instances, merged track %u triangles, %zu textures (%.1f MB)",
              rays[0], rays[1], rays[2], rays[2] ? rays[3] / 1000.0 / rays[2] : 0.0, gpu, all, g_tlas_ms, g_inst.size(), g_merged ? g_merged->blas->tris : 0, g_texinfo.size(), g_texels.size() * 4 / 1048576.0);
    }
    return tex;
}
