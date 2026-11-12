// Ray-tracing scene capture (see rt_scene.h).
//
// Step 1: FVR_RT_DUMP=<seconds> captures the scene pass of the first frame after that time, builds
// a tinybvh BVH over all of it on the CPU and traces one primary ray per pixel from the game's
// camera into rt_dump.ppm (FVR_SHOT_DIR or the current folder). When the silhouettes match the
// game's own frame, the capture (geometry, matrices) is right.
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
#include <algorithm>

#define TINYBVH_IMPLEMENTATION
#include "../third_party/tiny_bvh.h"

static std::vector<tinybvh::bvhvec4> g_tris;   // world-space triangle soup, 3 vertices each
// draws whose matrix needs the camera: object-space triangles and their object -> clip matrix
struct Pending { size_t first, count; float m[16]; };
static std::vector<tinybvh::bvhvec4> g_obj;
static std::vector<Pending> g_pending;
static std::map<std::string, uint64_t> g_skip_names;
static uint64_t g_frame = ~0ull;
static int g_state;                            // 0 waiting, 1 capturing g_frame, 2 done
static uint64_t g_draws, g_skips[RT_SKIP_N], g_badtri;

static double dump_time() {
    static double t = -2;
    if (t == -2) { const char *e = getenv("FVR_RT_DUMP"); t = e ? atof(e) : -1; }
    return t;
}

bool rt_capture_active(uint64_t frame) {
    if (g_state == 2 || dump_time() < 0) return false;
    if (g_state == 0) {
        if (SDL_GetTicks() / 1000.0 < dump_time()) return false;
        g_state = 1; g_frame = frame;
    }
    return frame == g_frame;
}

void rt_skip(RtSkip why, const char *what) { g_skips[why]++; if (what) g_skip_names[what]++; }

static inline void xform(const float *w, float x, float y, float z, tinybvh::bvhvec4 &o) {
    o = tinybvh::bvhvec4(w[0] * x + w[1] * y + w[2] * z + w[3],
                         w[4] * x + w[5] * y + w[6] * z + w[7],
                         w[8] * x + w[9] * y + w[10] * z + w[11], 0.0f);
}

void rt_capture(const RtDraw &d) {
    if (d.pos_type != 2 && d.pos_type != 3) { g_skips[RT_SKIP_FORMAT]++; return; }
    if (d.prim != 4 && d.prim != 5) { g_skips[RT_SKIP_PRIM]++; return; }
    uint32_t nidx = d.prim == 4 ? d.prims * 3 : d.prims + 2;
    auto vtx = [&](uint32_t k, tinybvh::bvhvec4 &o) -> bool {
        int64_t v;
        if (d.ib) v = (int64_t)(d.i32 ? ((const uint32_t *)d.ib)[d.first + k] : ((const uint16_t *)d.ib)[d.first + k]) + d.base_vertex;
        else v = (int64_t)d.first + k;
        if (v < 0 || (uint64_t)v * d.stride + 12 > d.vb_size) return false;
        float p[3]; memcpy(p, d.pos + v * d.stride, 12);
        if (d.wvp) o = tinybvh::bvhvec4(p[0], p[1], p[2], 1.0f);
        else xform(d.m, p[0], p[1], p[2], o);
        return true;
    };
    tinybvh::bvhvec4 a, b, c;
    std::vector<tinybvh::bvhvec4> &out = d.wvp ? g_obj : g_tris;
    size_t start = out.size();
    for (uint32_t t = 0; t < d.prims; t++) {
        uint32_t i0, i1, i2;
        if (d.prim == 4) { i0 = t * 3; i1 = i0 + 1; i2 = i0 + 2; }
        else { i0 = t; i1 = t + 1 + (t & 1); i2 = t + 2 - (t & 1); }
        if (i2 >= nidx || !vtx(i0, a) || !vtx(i1, b) || !vtx(i2, c)) { g_badtri++; continue; }
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
    if (d.wvp) { Pending p; p.first = start; p.count = out.size() - start; memcpy(p.m, d.m, 64); g_pending.push_back(p); }
    g_draws++;
}

static void mul4(const float *a, const float *b, float *r) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = v; }
}
static void dump(const float *ivp) {
    for (auto &p : g_pending) {   // world = inverse(camera) * WorldViewProj
        float w[16]; mul4(ivp, p.m, w);
        for (size_t i = p.first; i < p.first + p.count; i++) {
            const tinybvh::bvhvec4 &o = g_obj[i];
            float v[4];
            for (int r = 0; r < 4; r++) v[r] = w[r * 4] * o.x + w[r * 4 + 1] * o.y + w[r * 4 + 2] * o.z + w[r * 4 + 3];
            g_tris.push_back(tinybvh::bvhvec4(v[0] / v[3], v[1] / v[3], v[2] / v[3], 0.0f));
        }
    }
    uint32_t n = (uint32_t)(g_tris.size() / 3);
    std::vector<std::pair<uint64_t, std::string>> top;
    for (auto &k : g_skip_names) top.push_back({k.second, k.first});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 12; i++) rtlog("rt: skipped %llu draws of shader [%s]", (unsigned long long)top[i].first, top[i].second.c_str());
    rtlog("rt: %llu triangles had vertices outside their buffer", (unsigned long long)g_badtri);
    rtlog("rt: captured %llu draws, %u triangles; skipped: shader %llu, instanced %llu, UP %llu, format %llu, prim %llu, no camera %llu",
          (unsigned long long)g_draws, n, (unsigned long long)g_skips[RT_SKIP_SHADER], (unsigned long long)g_skips[RT_SKIP_INSTANCED],
          (unsigned long long)g_skips[RT_SKIP_UP], (unsigned long long)g_skips[RT_SKIP_FORMAT], (unsigned long long)g_skips[RT_SKIP_PRIM],
          (unsigned long long)g_skips[RT_SKIP_NOCAM]);
    if (n < 4) return;
    auto t0 = std::chrono::steady_clock::now();
    tinybvh::BVH bvh;
    bvh.Build(g_tris.data(), n);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rtlog("rt: BVH built in %.1f ms", ms);
    const int W = 960, H = 540;
    std::vector<uint8_t> img(W * H * 3);
    auto unproj = [&](float x, float y, float z, float *o) {
        float v[4];
        for (int i = 0; i < 4; i++) v[i] = ivp[i * 4] * x + ivp[i * 4 + 1] * y + ivp[i * 4 + 2] * z + ivp[i * 4 + 3];
        o[0] = v[0] / v[3]; o[1] = v[1] / v[3]; o[2] = v[2] / v[3];
    };
    t0 = std::chrono::steady_clock::now();
    for (int py = 0; py < H; py++)
        for (int px = 0; px < W; px++) {
            float x = (px + 0.5f) / W * 2 - 1, y = 1 - (py + 0.5f) / H * 2;
            float a[3], b[3];
            unproj(x, y, 0.0f, a); unproj(x, y, 1.0f, b);
            tinybvh::bvhvec3 O(a[0], a[1], a[2]), Dv(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            float len = sqrtf(Dv.x * Dv.x + Dv.y * Dv.y + Dv.z * Dv.z);
            tinybvh::Ray r(O, Dv * (1.0f / len), len);
            bvh.Intersect(r);
            uint8_t *o = &img[(py * W + px) * 3];
            if (r.hit.t >= len) { o[0] = 90; o[1] = 120; o[2] = 160; continue; }   // sky
            const tinybvh::bvhvec4 &v0 = g_tris[r.hit.prim * 3], &v1 = g_tris[r.hit.prim * 3 + 1], &v2 = g_tris[r.hit.prim * 3 + 2];
            tinybvh::bvhvec3 e1(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z), e2(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z);
            tinybvh::bvhvec3 nn = tinybvh::tinybvh_cross(e1, e2);
            float nl = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z) + 1e-20f;
            float sh = fabsf((nn.x * r.D.x + nn.y * r.D.y + nn.z * r.D.z) / nl);
            float up = fabsf(nn.y / nl);
            float fog = expf(-r.hit.t * 0.0015f);
            o[0] = (uint8_t)(255 * fog * (0.15f + 0.6f * sh));
            o[1] = (uint8_t)(255 * fog * (0.15f + 0.6f * sh * (0.6f + 0.4f * up)));
            o[2] = (uint8_t)(255 * fog * (0.15f + 0.45f * sh));
        }
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rtlog("rt: traced %dx%d primary rays on the CPU in %.0f ms", W, H, ms);
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
    if (g_state != 1 || frame != g_frame) return;
    g_state = 2;
    dump(ivp);
    g_tris.clear(); g_tris.shrink_to_fit(); g_obj.clear(); g_obj.shrink_to_fit(); g_pending.clear();
}
