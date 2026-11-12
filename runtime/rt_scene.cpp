// Ray-tracing scene (see rt_scene.h).
//
// FVR_RT=1 (or rt=1 in options.txt) keeps the acceleration structures up to date every frame and,
// with FVR_RT_STATS=1, logs what they cost once a second. FVR_RT_DUMP=<seconds> traces one frame on
// the CPU through the same two levels (TLAS, then each instance's CWBVH) from the game's camera into
// rt_dump.ppm (FVR_SHOT_DIR or the current folder): when the picture matches the game's own frame,
// geometry, matrices and both levels are right.
#include "rt_scene.h"
#include "rt.h"
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

#define TINYBVH_IMPLEMENTATION
#include "../third_party/tiny_bvh.h"

using tinybvh::bvhvec3;
using tinybvh::bvhvec4;
using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---------------------------------------------------------------- BLAS cache
struct MeshKey {
    uint32_t vb, vbv, ib, ibv, stride, first, prims;
    int32_t base;
    uint8_t prim, type, i32;
    bool operator==(const MeshKey &o) const {
        return vb == o.vb && vbv == o.vbv && ib == o.ib && ibv == o.ibv && stride == o.stride && first == o.first &&
               prims == o.prims && base == o.base && prim == o.prim && type == o.type && i32 == o.i32;
    }
};
struct MeshKeyHash {
    size_t operator()(const MeshKey &k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(k.vb); mix(k.vbv); mix(k.ib); mix(k.ibv); mix(k.stride); mix(k.first); mix(k.prims);
        mix((uint32_t)k.base); mix(k.prim | k.type << 8 | k.i32 << 16);
        return (size_t)h;
    }
};
// the same draw range regardless of buffer versions: buffers rewritten every frame are not worth a BLAS
static MeshKey slot_of(MeshKey k) { k.vbv = k.ibv = 0; return k; }

struct Blas {
    tinybvh::BVH8_CWBVH bvh;
    std::vector<bvhvec4> v;      // object-space positions
    std::vector<uint32_t> idx;   // 3 per triangle (padded to at least 4 triangles)
    uint32_t tris = 0;
    bvhvec3 lo{1e30f}, hi{-1e30f};
    uint64_t used = 0;           // last frame drawn
};
static std::unordered_map<MeshKey, std::unique_ptr<Blas>, MeshKeyHash> g_blas;
struct SlotInfo { uint32_t vbv, ibv, changes; };
static std::unordered_map<MeshKey, SlotInfo, MeshKeyHash> g_slots;

// ---------------------------------------------------------------- this frame's instances
struct Inst {
    Blas *b;
    float m[16];                 // as captured (object -> world 3 rows, or object -> clip)
    bool wvp;
    const char *shader;
    float w[12], iw[12];         // object -> world and world -> object (3x4 rows)
};
static std::vector<Inst> g_inst;
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
    auto vertex = [&](int64_t v, uint32_t &out) -> bool {
        auto it = remap.find(v);
        if (it != remap.end()) { out = it->second; return true; }
        if (v < 0 || (uint64_t)v * d.stride + 12 > d.vb_size) return false;
        float p[3]; memcpy(p, d.pos + v * d.stride, 12);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
        out = (uint32_t)b.v.size();
        b.v.push_back(bvhvec4(p[0], p[1], p[2], 0));
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
    MeshKey k{d.vb_id, d.vb_ver, d.ib_id, d.ib_ver, d.stride, d.first, d.prims, d.base_vertex,
              (uint8_t)d.prim, d.pos_type, (uint8_t)d.i32};
    Blas *b = nullptr;
    auto it = g_blas.find(k);
    if (it != g_blas.end()) b = it->second.get();
    else {
        SlotInfo &s = g_slots[slot_of(k)];
        if (s.changes > 0 && (s.vbv != k.vbv || s.ibv != k.ibv)) s.changes++;
        else if (!s.changes) s.changes = 1;
        s.vbv = k.vbv; s.ibv = k.ibv;
        if (s.changes > 4) { g_skips[RT_SKIP_VOLATILE]++; return; }   // rewritten all the time (particles, debris)
        bool dump = g_dump_state == 1 && g_frame == g_dump_frame;
        if (!dump && g_build_ms > build_budget_ms()) { g_skips[RT_SKIP_BUDGET]++; return; }   // next frame
        auto t0 = Clock::now();
        auto nb = std::make_unique<Blas>();
        bool ok = fill_blas(d, *nb);
        g_build_ms += ms_since(t0);
        if (!ok) { g_skips[RT_SKIP_EMPTY]++; return; }
        g_new_blas++; g_new_tris += nb->tris;
        b = nb.get();
        g_blas.emplace(k, std::move(nb));
    }
    b->used = g_frame;
    Inst in;
    in.b = b; memcpy(in.m, d.m, 64); in.wvp = d.wvp; in.shader = d.shader;
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
static bool build_tlas(const float *ivp) {
    g_tlas_aabbs.clear();
    size_t n = 0;
    for (size_t i = 0; i < g_inst.size(); i++) {
        Inst &in = g_inst[i];
        if (in.wvp) {   // world = inverse(camera) * WorldViewProj; the bottom row is (0 0 0 1) up to rounding
            float w[16]; mul4(ivp, in.m, w);
            float s = fabsf(w[15]) > 1e-12f ? 1.0f / w[15] : 1.0f;
            for (int k = 0; k < 12; k++) in.w[k] = w[k] * s;
        } else memcpy(in.w, in.m, 48);
        if (!inv34(in.w, in.iw)) continue;
        bvhvec3 lo(1e30f), hi(-1e30f);
        for (int c = 0; c < 8; c++) {
            bvhvec3 p((c & 1) ? in.b->hi.x : in.b->lo.x, (c & 2) ? in.b->hi.y : in.b->lo.y, (c & 4) ? in.b->hi.z : in.b->lo.z);
            p = xp(in.w, p);
            lo = tinybvh::tinybvh_min(lo, p); hi = tinybvh::tinybvh_max(hi, p);
        }
        g_inst[n++] = in;
        g_tlas_aabbs.push_back(bvhvec4(lo.x, lo.y, lo.z, 0));
        g_tlas_aabbs.push_back(bvhvec4(hi.x, hi.y, hi.z, 0));
    }
    g_inst.resize(n);
    if (n < 2) return false;
    g_tlas.BuildAABB(g_tlas_aabbs.data(), (uint32_t)n);
    return true;
}

// Frees BLASes that have not been drawn for a while (meshes of other places on the track stay).
static void evict(uint64_t frame) {
    if (frame % 600) return;
    for (auto it = g_blas.begin(); it != g_blas.end();)
        if (frame - it->second->used > 3600) it = g_blas.erase(it); else ++it;
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
    (void)vp;
    if (frame != g_frame) return;
    auto t0 = Clock::now();
    bool ok = build_tlas(ivp);
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
