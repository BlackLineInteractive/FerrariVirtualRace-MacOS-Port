// Metal side of the ray tracer (see rt_metal.h). The traversal kernels follow the CWBVH (BVH8)
// and BVH2 TLAS walk of an existing Metal path tracer (Scene.metal), on tinybvh's data layouts.
#define GL_SILENCE_DEPRECATION
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <Foundation/Foundation.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/gl3.h>
#include "rt_metal.h"
#include "rt.h"
#include <cstring>
#include <atomic>

static const char *kSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Inst { float4 r0, r1, r2; uint nodeBase, triBase, attrBase, flags; float4 color, uvm0, uvm1; int tex; uint pad0, pad1, pad2; };
struct TriAttr { float2 uv0, uv1, uv2; uint c0, c1, c2; uint tex; };
struct TexInfo { uint off, w, h, pad; };
struct Light { float4 sunDir, sunCol, ambLo, ambHi; float ambLevel, p0, p1, p2; };
struct TNode { float4 mn; float4 mx; };          // .w: leftFirst / triCount (uint bits)
struct Params { float4 ivp[4]; uint w, h, nInst, mode; };   // mode 2: look backwards

constant uint kCwStack = 32;
constant uint kTlasStack = 32;

// Four children of a CWBVH node against the ray: bits for the ones it meets.
inline uint cwTestQuad(uint meta4, uint loxSource, uint hixSource, uint loySource, uint hiySource, uint lozSource, uint hizSource,
                       bool3 negDir, uint octinv, float3 adjustedIdir, float3 orig, float tmin, float tMax) {
    const uint isInner4   = (meta4 & (meta4 << 1u)) & 0x10101010u;
    const uint innerMask4 = as_type<uint>(as_type<char4>(isInner4 << 3u) >> 7);
    const uint bitIndex4  = (meta4 ^ (octinv & innerMask4)) & 0x1F1F1F1Fu;
    const uint childBits4 = (meta4 >> 5u) & 0x07070707u;
    const float4 lox = float4(as_type<uchar4>(negDir.x ? hixSource : loxSource));
    const float4 loy = float4(as_type<uchar4>(negDir.y ? hiySource : loySource));
    const float4 loz = float4(as_type<uchar4>(negDir.z ? hizSource : lozSource));
    const float4 hix = float4(as_type<uchar4>(negDir.x ? loxSource : hixSource));
    const float4 hiy = float4(as_type<uchar4>(negDir.y ? loySource : hiySource));
    const float4 hiz = float4(as_type<uchar4>(negDir.z ? lozSource : hizSource));
    const float4 cmin = max(max(max(lox * adjustedIdir.x + orig.x, loy * adjustedIdir.y + orig.y),
                                loz * adjustedIdir.z + orig.z), tmin);
    const float4 cmax = min(min(min(hix * adjustedIdir.x + orig.x, hiy * adjustedIdir.y + orig.y),
                                hiz * adjustedIdir.z + orig.z), tMax);
    const uint4 contribution = uint4(as_type<uchar4>(childBits4)) << uint4(as_type<uchar4>(bitIndex4));
    const uint4 accepted = select(uint4(0u), contribution, cmin <= cmax);
    return accepted.x | accepted.y | accepted.z | accepted.w;
}

struct Hit { float t; float3 n; uint inst, prim; float u, v; };    // n: object-space geometric normal (unnormalised)

bool intersectBlas(device const float4 *nodes, device const float4 *tris, uint nodeBase, uint triBase,
                   float3 O, float3 D, float3 rD, float tMin, thread float &tMax, thread Hit &hit, uint instance, bool anyHit) {
    uint stack[kCwStack];
    uint sp = 0;
    bool found = false;
    const bool3  negDir = rD < 0.0f;
    const float3 Ord   = O * rD;
    const uint octinv = (7u - ((D.x < 0.0f ? 4u : 0u) | (D.y < 0.0f ? 2u : 0u) | (D.z < 0.0f ? 1u : 0u))) * 0x01010101u;
    uint2 ngroup = uint2(0u, 0x80000000u);
    uint2 tgroup = uint2(0u, 0u);
    while (true) {
        if (ngroup.y > 0x00FFFFFFu) {
            const uint imask = ngroup.y;
            const uint childBit = 31u - clz(imask);
            const uint childBase = ngroup.x;
            ngroup.y &= ~(1u << childBit);
            if (ngroup.y > 0x00FFFFFFu && sp + 2u <= kCwStack) { stack[sp++] = ngroup.x; stack[sp++] = ngroup.y; }
            const uint slot = (childBit - 24u) ^ (octinv & 255u);
            const uint base = nodeBase + (childBase + popcount(imask & ~(0xFFFFFFFFu << slot))) * 5u;
            const float4 n0 = nodes[base], n1 = nodes[base + 1u], n2 = nodes[base + 2u];
            const float4 n3 = nodes[base + 3u], n4 = nodes[base + 4u];
            const char4 exponents = as_type<char4>(n0.w);
            ngroup.x = as_type<uint>(n1.x);
            tgroup.x = as_type<uint>(n1.y);
            const float3 adjustedIdir = ldexp(rD, int3(exponents.xyz));
            const float3 orig = n0.xyz * rD - Ord;
            const uint hitmask = cwTestQuad(as_type<uint>(n1.z), as_type<uint>(n2.x), as_type<uint>(n3.z), as_type<uint>(n2.z),
                                            as_type<uint>(n4.x), as_type<uint>(n3.x), as_type<uint>(n4.z),
                                            negDir, octinv, adjustedIdir, orig, tMin, tMax) |
                                 cwTestQuad(as_type<uint>(n1.w), as_type<uint>(n2.y), as_type<uint>(n3.w), as_type<uint>(n2.w),
                                            as_type<uint>(n4.y), as_type<uint>(n3.y), as_type<uint>(n4.w),
                                            negDir, octinv, adjustedIdir, orig, tMin, tMax);
            ngroup.y = (hitmask & 0xFF000000u) | (as_type<uint>(n0.w) >> 24u);
            tgroup.y = hitmask & 0x00FFFFFFu;
        } else {
            tgroup = ngroup;
            ngroup = uint2(0u, 0u);
        }
        while (tgroup.y != 0u) {
            const uint triangle = 31u - clz(tgroup.y);
            tgroup.y -= 1u << triangle;
            const uint at = triBase + tgroup.x + triangle * 3u;
            const float3 e2 = tris[at].xyz, e1 = tris[at + 1u].xyz;
            const float4 v0 = tris[at + 2u];
            const float3 h = cross(D, e2);
            const float a = dot(e1, h);
            if (abs(a) < 1e-12f) continue;
            const float f = 1.0f / a;
            const float3 s = O - v0.xyz;
            const float u = f * dot(s, h);
            if (u < 0.0f || u > 1.0f) continue;
            const float3 q = cross(s, e1);
            const float v = f * dot(D, q);
            if (v < 0.0f || u + v > 1.0f) continue;
            const float t = f * dot(e2, q);
            if (t <= tMin || t >= tMax) continue;
            tMax = t;
            hit.t = t; hit.n = cross(e1, e2); hit.inst = instance; hit.prim = as_type<uint>(v0.w) & 0x7FFFFFFFu; hit.u = u; hit.v = v;
            found = true;
            if (anyHit) return true;
        }
        if (ngroup.y > 0x00FFFFFFu) continue;
        if (sp == 0u) break;
        ngroup.y = stack[--sp];
        ngroup.x = stack[--sp];
    }
    return found;
}

bool traceTlas(device const float4 *nodes, device const float4 *tris, device const float4 *mnodes, device const float4 *mtris,
               device const TNode *tlas, device const uint *prims, device const Inst *insts, float3 origin, float3 direction, float tMin, thread float &tMax, thread Hit &hit,
               bool anyHit = false) {
    const float3 rD = 1.0f / select(direction, float3(1e-20f), abs(direction) < 1e-20f);
    uint stack[kTlasStack];
    uint sp = 0, nodeIdx = 0;
    bool found = false;
    while (true) {
        const TNode node = tlas[nodeIdx];
        const uint leftFirst = as_type<uint>(node.mn.w);
        const uint triCount  = as_type<uint>(node.mx.w);
        if (triCount > 0u) {
            for (uint i = 0u; i < triCount; i++) {
                const uint ii = prims[leftFirst + i];
                const Inst in = insts[ii];
                const float4 o4 = float4(origin, 1.0f), d4 = float4(direction, 0.0f);
                const float3 O = float3(dot(in.r0, o4), dot(in.r1, o4), dot(in.r2, o4));
                const float3 D = float3(dot(in.r0, d4), dot(in.r1, d4), dot(in.r2, d4));
                const float3 iD = 1.0f / select(D, float3(1e-20f), abs(D) < 1e-20f);
                if ((in.flags & 1u) ? intersectBlas(mnodes, mtris, 0u, 0u, O, D, iD, tMin, tMax, hit, ii, anyHit)
                                    : intersectBlas(nodes, tris, in.nodeBase, in.triBase, O, D, iD, tMin, tMax, hit, ii, anyHit)) {
                    found = true;
                    if (anyHit) return true;
                }
            }
        } else {
            const uint childA = leftFirst, childB = leftFirst + 1u;
            const TNode a = tlas[childA], b = tlas[childB];
            float3 t1 = (a.mn.xyz - origin) * rD, t2 = (a.mx.xyz - origin) * rD;
            float3 lo = min(t1, t2), hi = max(t1, t2);
            float tA = max(max(max(lo.x, lo.y), lo.z), tMin), hA = min(min(min(hi.x, hi.y), hi.z), tMax);
            float distA = (tA > hA) ? 1e30f : tA;
            t1 = (b.mn.xyz - origin) * rD; t2 = (b.mx.xyz - origin) * rD;
            lo = min(t1, t2); hi = max(t1, t2);
            float tB = max(max(max(lo.x, lo.y), lo.z), tMin), hB = min(min(min(hi.x, hi.y), hi.z), tMax);
            float distB = (tB > hB) ? 1e30f : tB;
            uint nearC = childA, farC = childB;
            if (distB < distA) { nearC = childB; farC = childA; const float s = distA; distA = distB; distB = s; }
            if (distA < 1e30f) {
                if (distB < 1e30f && sp < kTlasStack) stack[sp++] = farC;
                nodeIdx = nearC;
                continue;
            }
        }
        if (sp == 0u) break;
        nodeIdx = stack[--sp];
    }
    return found;
}

struct RParams { float4 cam; uint w, h, nInst, pad; };

// Textures: RGBA8 in one buffer, bilinear, repeating (as the path tracer's sampleTex).
inline float4 texel(device const uint *tx, TexInfo t, int x, int y) {
    const uint c = tx[t.off + uint(y) * t.w + uint(x)];
    return float4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) / 255.0f;
}
float4 sampleTex(device const uint *tx, device const TexInfo *ti, int id, float2 uv) {
    const TexInfo t = ti[id];
    const float fx = uv.x * float(t.w) - 0.5f, fy = uv.y * float(t.h) - 0.5f;
    const float flx = floor(fx), fly = floor(fy);
    const float ax = fx - flx, ay = fy - fly;
    const int W = int(t.w), H = int(t.h);
    int x0 = int(flx - floor(flx / float(W)) * float(W)), y0 = int(fly - floor(fly / float(H)) * float(H));
    x0 = clamp(x0, 0, W - 1); y0 = clamp(y0, 0, H - 1);
    const int x1 = x0 + 1 == W ? 0 : x0 + 1, y1 = y0 + 1 == H ? 0 : y0 + 1;
    return mix(mix(texel(tx, t, x0, y0), texel(tx, t, x1, y0), ax), mix(texel(tx, t, x0, y1), texel(tx, t, x1, y1), ax), ay);
}
inline float3 unpack8(uint c) { return float3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) / 255.0f; }

struct SceneB {
    device const float4 *nodes, *tris, *mnodes, *mtris;
    device const TNode *tlas;
    device const uint *prims;
    device const Inst *insts;
    device const TriAttr *attrs, *mattrs;
    device const uint *texels;
    device const TexInfo *texinfo;
    uint nTex, nInst;
};
inline bool trace(const thread SceneB &sc, float3 O, float3 D, float tMin, thread float &tMax, thread Hit &hit, bool anyHit = false) {
    return sc.nInst > 0u && traceTlas(sc.nodes, sc.tris, sc.mnodes, sc.mtris, sc.tlas, sc.prims, sc.insts, O, D, tMin, tMax, hit, anyHit);
}
inline float3 hitNormal(const thread SceneB &sc, const thread Hit &hit) {   // world space, unnormalised direction fixed
    const Inst in = sc.insts[hit.inst];
    return normalize(hit.n.x * in.r0.xyz + hit.n.y * in.r1.xyz + hit.n.z * in.r2.xyz);
}

// A hit shaded as the game shades it: the track with its texture, sun and baked light
// (COLOR1^2 * AmbLevel), car materials with their colour, sun and hemisphere ambient (x2);
// the sun's shadow by a shadow ray.
float3 shadeHit(const thread SceneB &sc, constant Light &L, const thread Hit &hit, float3 X, float3 D) {
    const Inst in = sc.insts[hit.inst];
    const bool merged = (in.flags & 1u) != 0u;
    const TriAttr a = merged ? sc.mattrs[hit.prim] : sc.attrs[in.attrBase + hit.prim];
    const float w0 = 1.0f - hit.u - hit.v;
    float2 uv = a.uv0 * w0 + a.uv1 * hit.u + a.uv2 * hit.v;
    if (!merged && (in.flags & 4u) != 0u) uv = float2(dot(in.uvm0.xyz, float3(uv, 1.0f)), dot(in.uvm1.xyz, float3(uv, 1.0f)));
    const uint tid = (a.tex & 0x7FFFFFFFu) != 0x7FFFFFFFu ? (a.tex & 0x7FFFFFFFu) : uint(in.tex);
    float3 albedo = in.color.rgb;
    if (int(tid) >= 0 && tid < sc.nTex) albedo *= sampleTex(sc.texels, sc.texinfo, int(tid), uv).rgb;
    float3 n = hitNormal(sc, hit);
    if (dot(n, D) > 0.0f) n = -n;
    const float3 S = normalize(L.sunDir.xyz);
    const float NL = saturate(dot(n, S));
    float sh = 0.0f;
    if (NL > 0.0f) {
        float tS = 1e5f;
        Hit h2; h2.t = tS; h2.inst = 0xFFFFFFFFu;
        sh = trace(sc, X + n * (0.002f * hit.t + 0.02f), S, 0.0f, tS, h2, true) ? 0.0f : 1.0f;
    }
    const float3 sun = L.sunCol.rgb * NL * sh;
    if ((in.flags & 2u) != 0u) {
        const float up = saturate(n.y);
        return 2.0f * albedo * (sun + mix(L.ambLo.rgb, L.ambHi.rgb, up * up));
    }
    float3 baked;
    if ((a.tex & 0x80000000u) != 0u) {
        const float3 k = unpack8(a.c0) * w0 + unpack8(a.c1) * hit.u + unpack8(a.c2) * hit.v;
        baked = k * k * L.ambLevel;
    } else baked = float3(0.26f, 0.34f, 0.41f) * L.ambLevel;   // the track's average sky light
    return albedo * (sun + baked);
}

// What is seen through glass from X along D. refract: bend at every glass surface (IOR 1.5, total
// internal reflection inside); otherwise go straight through glass. thick: distance between the
// first two glass surfaces of the refracted path (0: none, stats).
float3 seeThrough(const thread SceneB &sc, constant Light &L, float3 X, float3 D, bool refractOn, thread float &thick) {
    bool inside = refractOn;   // the ray starts just inside the first surface when refracting
    for (int k = 0; k < 6; k++) {
        float tMax = 1e5f;
        Hit h; h.t = tMax; h.inst = 0xFFFFFFFFu; h.prim = 0u; h.u = h.v = 0.0f; h.n = float3(0, 1, 0);
        if (!trace(sc, X, D, 0.003f, tMax, h)) return L.ambHi.rgb;   // sky (approximately)
        const float3 P = X + D * h.t;
        const Inst in = sc.insts[h.inst];
        if ((in.flags & 8u) == 0u) {
            if (refractOn && inside) { thick = -1.0f; return float3(0.0f); }   // never left the glass: it is a single sheet
            return shadeHit(sc, L, h, P, D);
        }
        if (refractOn) {
            if (inside) thick = max(thick, 1e-6f);   // a second surface: the glass has a thickness
            if (k == 0 && inside) thick = h.t;
            float3 n = hitNormal(sc, h);
            if (dot(n, D) > 0.0f) n = -n;                 // against the ray
            const float eta = inside ? 1.5f : 1.0f / 1.5f;
            float3 T = refract(D, n, eta);
            if (all(T == 0.0f)) T = reflect(D, n); else inside = !inside;
            D = normalize(T);
        }
        X = P + D * 0.002f;
    }
    return float3(0.0f);
}

// Reflection rays, only where the material asked for one (mask written by the PBR car shader):
// m0 = (reflection direction, GGX alpha; for glass -transmittance), m1 = (position - camera,
// weight; 0 = no ray). out = (hit position, 1) for a hit, (0, 0, 0, 2) for a miss (sky), 0 where
// nothing was traced; col = the hit shaded; tcol (glass only) = transmittance x (what refraction
// shows - what is straight behind), the correction the composite adds to the game's own image.
kernel void rt_reflect(device const float4 *nodes [[buffer(0)]], device const float4 *tris [[buffer(1)]],
                       device const TNode *tlas [[buffer(2)]], device const uint *prims [[buffer(3)]],
                       device const Inst *insts [[buffer(4)]], constant RParams &P [[buffer(5)]],
                       device const float4 *mnodes [[buffer(6)]], device const float4 *mtris [[buffer(7)]],
                       device atomic_uint *count [[buffer(8)]],
                       device const TriAttr *attrs [[buffer(9)]], device const TriAttr *mattrs [[buffer(10)]],
                       device const uint *texels [[buffer(11)]], device const TexInfo *texinfo [[buffer(12)]],
                       constant Light &L [[buffer(13)]], constant uint &nTex [[buffer(14)]],
                       texture2d<float, access::read> m0 [[texture(0)]], texture2d<float, access::read> m1 [[texture(1)]],
                       texture2d<float, access::write> out [[texture(2)]], texture2d<half, access::write> col [[texture(3)]],
                       texture2d<half, access::write> tcol [[texture(4)]],
                       uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= P.w || gid.y >= P.h) return;
    const float4 b = m1.read(gid);
    tcol.write(half4(0.0h), gid);
    if (!(b.w > 0.0f)) { out.write(float4(0.0f), gid); return; }
    SceneB sc = {nodes, tris, mnodes, mtris, tlas, prims, insts, attrs, mattrs, texels, texinfo, nTex, P.nInst};
    atomic_fetch_add_explicit(&count[0], 1u, memory_order_relaxed);
    const float4 mm = m0.read(gid);
    const float3 R = normalize(mm.xyz);
    const float3 O = P.cam.xyz + b.xyz;
    const float tMin = 0.002f * length(b.xyz) + 0.01f;
    // glass: refraction minus the straight view, weighted by how much light the glass lets through
    if (mm.w < 0.0f && P.pad != 0u) {
        const float3 Ve = -normalize(b.xyz);              // to the eye
        const float3 N = normalize(R + Ve);               // the surface normal, facing the eye
        const float3 T = refract(-Ve, N, 1.0f / 1.5f);
        float thick = 0.0f, dummy = 0.0f;
        const float3 Lr = seeThrough(sc, L, O - N * 0.002f, all(T == 0.0f) ? -Ve : normalize(T), true, thick);
        const float3 Ls = seeThrough(sc, L, O - N * 0.002f, -Ve, false, dummy);
        // a single sheet of glass (no second surface) is thin: light leaves it parallel, no change
        if (thick >= 0.0f) tcol.write(half4(half3((Lr - Ls) * -mm.w), 1.0h), gid);
        atomic_fetch_add_explicit(&count[1], 1u, memory_order_relaxed);
        if (thick > 1e-5f) {
            atomic_fetch_add_explicit(&count[2], 1u, memory_order_relaxed);
            atomic_fetch_add_explicit(&count[3], uint(min(thick, 100.0f) * 1000.0f), memory_order_relaxed);
        }
    }
    float tMax = 1e5f;
    Hit hit; hit.t = tMax; hit.n = float3(0, 1, 0); hit.inst = 0xFFFFFFFFu; hit.prim = 0u; hit.u = hit.v = 0.0f;
    if (!trace(sc, O, R, tMin, tMax, hit)) { out.write(float4(0.0f, 0.0f, 0.0f, 2.0f), gid); return; }
    const float3 X = O + R * hit.t;
    out.write(float4(X, 1.0f), gid);
    col.write(half4(half3(shadeHit(sc, L, hit, X, R)), 1.0h), gid);
}

// Debug view: one primary ray per pixel, shaded by the angle to the surface, one tint per instance.
kernel void rt_view(device const float4 *nodes [[buffer(0)]], device const float4 *tris [[buffer(1)]],
                    device const TNode *tlas [[buffer(2)]], device const uint *prims [[buffer(3)]],
                    device const Inst *insts [[buffer(4)]], constant Params &P [[buffer(5)]],
                    device const float4 *mnodes [[buffer(6)]], device const float4 *mtris [[buffer(7)]],
                    texture2d<half, access::write> out [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= P.w || gid.y >= P.h) return;
    const float x = (float(gid.x) + 0.5f) / float(P.w) * 2.0f - 1.0f;
    const float y = 1.0f - (float(gid.y) + 0.5f) / float(P.h) * 2.0f;
    const float4 pn = float4(x, y, 0.0f, 1.0f), pf = float4(x, y, 1.0f, 1.0f);
    const float4 a = float4(dot(P.ivp[0], pn), dot(P.ivp[1], pn), dot(P.ivp[2], pn), dot(P.ivp[3], pn));
    const float4 b = float4(dot(P.ivp[0], pf), dot(P.ivp[1], pf), dot(P.ivp[2], pf), dot(P.ivp[3], pf));
    const float3 O = a.xyz / a.w, E = b.xyz / b.w;
    float3 D = E - O;
    if (P.mode == 2u) D = float3(-D.x, D.y, -D.z);   // turned around (world is Y-up)
    const float len = length(D);
    D /= len;
    float tMax = len;
    Hit hit; hit.t = len; hit.n = float3(0, 1, 0); hit.inst = 0xFFFFFFFFu;
    half4 c;
    if (P.nInst == 0u || !traceTlas(nodes, tris, mnodes, mtris, tlas, prims, insts, O, D, 0.0f, tMax, hit)) {
        c = half4(0.35h, 0.47h, 0.63h, 1.0h);
    } else {
        const Inst in = insts[hit.inst];
        // object -> world for a normal: transpose of world -> object
        const float3 n = normalize(hit.n.x * in.r0.xyz + hit.n.y * in.r1.xyz + hit.n.z * in.r2.xyz);
        const float sh = abs(dot(n, D)), up = abs(n.y);
        const float fog = exp(-hit.t * 0.0015f);
        const uint hc = hit.inst * 2654435761u;
        const float3 tint = 0.75f + 0.25f * float3((hc >> 8) & 255u, (hc >> 16) & 255u, (hc >> 24) & 255u) / 255.0f;
        c = half4(half3(fog * tint * float3(0.15f + 0.6f * sh, 0.15f + 0.6f * sh * (0.6f + 0.4f * up), 0.15f + 0.45f * sh)), 1.0h);
    }
    out.write(c, gid);
}
)MSL";

namespace {
// An image shared by Metal and OpenGL through an IOSurface (GL sees a rectangle texture).
struct Img {
    IOSurfaceRef surf = nullptr;
    id<MTLTexture> tex;
    GLuint gl = 0;
    int w = 0, h = 0;
};
struct State {
    bool tried = false, ok = false;
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> view;
    id<MTLBuffer> nodes, tris;                 // BLAS data, grown as needed
    id<MTLBuffer> mnodes, mtris, mattr;        // the merged world-space BLAS
    id<MTLBuffer> attr, texels, texinfo;       // triangle attributes, textures
    uint32_t ntex = 0;
    id<MTLBuffer> frame[3];                    // instances, TLAS nodes, TLAS leaf indices
    Img vimg;                                  // debug view (RGBA16F)
    Img m0, m1, hit, hcol, tcol;               // reflection mask (from GL), hit points, their colour, glass refraction (to GL)
    id<MTLComputePipelineState> reflect;
    id<MTLBuffer> counter;
};
State S;
std::atomic<bool> g_ready{false};   // kernels compiled (in the background: it takes seconds the first time)

bool init() {
    if (S.tried) return S.ok;
    S.tried = true;
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        for (id<MTLDevice> d in all) {
            rtlog("rt: Metal device %s%s", d.name.UTF8String, d.lowPower ? " (low power)" : "");
            if (!S.dev || (S.dev.lowPower && !d.lowPower)) S.dev = d;
        }
        if (!S.dev) { rtlog("rt: no Metal device"); return false; }
        S.queue = [S.dev newCommandQueue];
        S.counter = [S.dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        S.ok = true;
        id<MTLDevice> dev = S.dev;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                auto t0 = [NSDate date];
                NSError *err = nil;
                MTLCompileOptions *opt = [MTLCompileOptions new];
                opt.mathMode = MTLMathModeFast;
                id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSource] options:opt error:&err];
                if (!lib) { rtlog("rt: Metal kernel compile failed: %s", err.localizedDescription.UTF8String); return; }
                id<MTLComputePipelineState> v = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rt_view"] error:&err];
                id<MTLComputePipelineState> r = v ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rt_reflect"] error:&err] : nil;
                if (!v || !r) { rtlog("rt: Metal pipeline failed: %s", err.localizedDescription.UTF8String); return; }
                S.view = v; S.reflect = r;
                g_ready = true;
                rtlog("rt: Metal ray tracing on %s (kernels compiled in %.1f s in the background)", dev.name.UTF8String, -[t0 timeIntervalSinceNow]);
            }
        });
    }
    return true;
}

// a managed buffer of at least `bytes`; contents are kept only if `keep` (copied by the caller otherwise)
id<MTLBuffer> ensure(id<MTLBuffer> b, size_t bytes, bool *grew) {
    *grew = false;
    if (b && b.length >= bytes) return b;
    size_t cap = b ? b.length : 1 << 16;
    while (cap < bytes) cap *= 2;
    *grew = true;
    return [S.dev newBufferWithLength:cap options:MTLResourceStorageModeManaged];
}
void upload(id<MTLBuffer> b, const void *src, size_t from, size_t to) {
    if (to <= from) return;
    memcpy((uint8_t *)b.contents + from, (const uint8_t *)src + from, to - from);
    [b didModifyRange:NSMakeRange(from, to - from)];
}

// half: RGBA16F, else RGBA32F
bool ensure_img(Img &m, int w, int h, bool half, const char *what) {
    if (m.surf && m.w == w && m.h == h) return true;
    if (m.gl) { glDeleteTextures(1, &m.gl); m.gl = 0; }
    if (m.surf) { CFRelease(m.surf); m.surf = nullptr; }
    m.tex = nil;
    NSDictionary *props = @{(id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(h), (id)kIOSurfaceBytesPerElement: @(half ? 8 : 16),
                            (id)kIOSurfacePixelFormat: @((uint32_t)(half ? 'RGhA' : 'RGfA'))};
    m.surf = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!m.surf) { rtlog("rt: IOSurface %dx%d failed", w, h); return false; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(half ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA32Float)
                                                                                 width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeManaged;
    m.tex = [S.dev newTextureWithDescriptor:d iosurface:m.surf plane:0];
    if (!m.tex) { rtlog("rt: Metal texture over IOSurface failed"); return false; }
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &prev);
    glGenTextures(1, &m.gl);
    glBindTexture(GL_TEXTURE_RECTANGLE, m.gl);
    CGLError e = CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, half ? GL_RGBA16F : GL_RGBA32F, w, h, GL_RGBA,
                                        half ? GL_HALF_FLOAT : GL_FLOAT, m.surf, 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_RECTANGLE, prev);
    if (e != kCGLNoError) { rtlog("rt: CGLTexImageIOSurface2D failed: %d", (int)e); glDeleteTextures(1, &m.gl); m.gl = 0; return false; }
    m.w = w; m.h = h;
    rtlog("rt: %s: %dx%d %s image shared between Metal and OpenGL", what, w, h, half ? "RGBA16F" : "RGBA32F");
    return true;
}
}   // namespace

bool rtm_available() { return init() && g_ready; }

void rtm_blas(const void *nodes, size_t n_nodes, size_t from_nodes, const void *tris, size_t n_tris, size_t from_tris) {
    if (!init()) return;
    bool grew;
    S.nodes = ensure(S.nodes, n_nodes * 16 + 16, &grew);
    upload(S.nodes, nodes, grew ? 0 : from_nodes * 16, n_nodes * 16);
    S.tris = ensure(S.tris, n_tris * 16 + 16, &grew);
    upload(S.tris, tris, grew ? 0 : from_tris * 16, n_tris * 16);
}

void rtm_attrs(const void *attr, size_t n, size_t from) {
    if (!init()) return;
    bool grew;
    S.attr = ensure(S.attr, n * 40 + 64, &grew);
    upload(S.attr, attr, grew ? 0 : from * 40, n * 40);
}
void rtm_textures(const uint32_t *texels, size_t n_texels, size_t from_texels, const void *info, size_t n_info) {
    if (!init()) return;
    bool grew;
    S.texels = ensure(S.texels, n_texels * 4 + 64, &grew);
    upload(S.texels, texels, grew ? 0 : from_texels * 4, n_texels * 4);
    S.texinfo = ensure(S.texinfo, n_info * 16 + 64, &grew);
    upload(S.texinfo, info, 0, n_info * 16);
    S.ntex = (uint32_t)n_info;
}
unsigned rtm_hit_colour() { return S.hcol.gl; }
unsigned rtm_glass() { return S.tcol.gl; }

void rtm_merged(const void *nodes, size_t n_nodes, const void *tris, size_t n_tris, const void *attr, size_t n_attr) {
    if (!init()) return;
    bool grew;
    S.mattr = ensure(S.mattr, n_attr * 40 + 64, &grew);
    upload(S.mattr, attr, 0, n_attr * 40);
    S.mnodes = ensure(S.mnodes, n_nodes * 16 + 16, &grew);
    upload(S.mnodes, nodes, 0, n_nodes * 16);
    S.mtris = ensure(S.mtris, n_tris * 16 + 16, &grew);
    upload(S.mtris, tris, 0, n_tris * 16);
}

unsigned rtm_trace_view(const RtmInst *inst, size_t n_inst, const void *tlas_nodes, size_t n_tlas, const uint32_t *prim, size_t n_prim,
                        const float *ivp, int w, int h, int mode, double *gpu_ms) {
    if (!init() || !g_ready || !S.nodes || !S.tris || !ensure_img(S.vimg, w, h, true, "debug view")) return 0;
    @autoreleasepool {
        const void *src[3] = {inst, tlas_nodes, prim};
        size_t bytes[3] = {n_inst * sizeof(RtmInst), n_tlas * 32, n_prim * 4};
        for (int i = 0; i < 3; i++) {
            bool grew;
            S.frame[i] = ensure(S.frame[i], bytes[i] + 64, &grew);
            upload(S.frame[i], src[i], 0, bytes[i]);
        }
        struct { float ivp[16]; uint32_t w, h, n, mode; } P;
        memcpy(P.ivp, ivp, 64);
        P.w = w; P.h = h; P.n = (uint32_t)n_inst; P.mode = (uint32_t)mode;
        id<MTLCommandBuffer> cb = [S.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:S.view];
        [enc setBuffer:S.nodes offset:0 atIndex:0];
        [enc setBuffer:S.tris offset:0 atIndex:1];
        [enc setBuffer:S.frame[1] offset:0 atIndex:2];
        [enc setBuffer:S.frame[2] offset:0 atIndex:3];
        [enc setBuffer:S.frame[0] offset:0 atIndex:4];
        [enc setBytes:&P length:sizeof P atIndex:5];
        [enc setBuffer:(S.mnodes ? S.mnodes : S.nodes) offset:0 atIndex:6];
        [enc setBuffer:(S.mtris ? S.mtris : S.tris) offset:0 atIndex:7];
        [enc setTexture:S.vimg.tex atIndex:0];
        [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            rtlog("rt: Metal command buffer failed: %s", cb.error.localizedDescription.UTF8String);
            return 0;
        }
        if (gpu_ms) *gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
    }
    return S.vimg.gl;
}

bool rtm_mask_textures(int w, int h, unsigned *gl_m0, unsigned *gl_m1) {
    if (!init()) return false;
    if (!ensure_img(S.m0, w, h, true, "reflection mask (direction, roughness)") ||
        !ensure_img(S.m1, w, h, false, "reflection mask (position, weight)")) return false;
    *gl_m0 = S.m0.gl; *gl_m1 = S.m1.gl;
    return true;
}

unsigned rtm_trace_reflect(const RtmInst *inst, size_t n_inst, const void *tlas_nodes, size_t n_tlas, const uint32_t *prim, size_t n_prim,
                           const float *cam, const RtmLight &light, bool refract, uint32_t *rays, double *gpu_ms) {
    if (!init() || !g_ready || !S.nodes || !S.tris || !S.m0.tex || !S.m1.tex) return 0;
    int w = S.m1.w, h = S.m1.h;
    if (!ensure_img(S.hit, w, h, false, "reflection hits") || !ensure_img(S.hcol, w, h, true, "reflection hit colours") ||
        !ensure_img(S.tcol, w, h, true, "glass refraction")) return 0;
    if (!S.attr || !S.texels || !S.texinfo) return 0;
    glFlush();   // the masks were just rendered by OpenGL
    @autoreleasepool {
        const void *src[3] = {inst, tlas_nodes, prim};
        size_t bytes[3] = {n_inst * sizeof(RtmInst), n_tlas * 32, n_prim * 4};
        for (int i = 0; i < 3; i++) {
            bool grew;
            S.frame[i] = ensure(S.frame[i], bytes[i] + 64, &grew);
            upload(S.frame[i], src[i], 0, bytes[i]);
        }
        struct { float cam[4]; uint32_t w, h, n, pad; } P;
        P.cam[0] = cam[0]; P.cam[1] = cam[1]; P.cam[2] = cam[2]; P.cam[3] = 0;
        P.w = w; P.h = h; P.n = (uint32_t)n_inst; P.pad = refract ? 1u : 0u;
        memset(S.counter.contents, 0, 16);
        id<MTLCommandBuffer> cb = [S.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:S.reflect];
        [enc setBuffer:S.nodes offset:0 atIndex:0];
        [enc setBuffer:S.tris offset:0 atIndex:1];
        [enc setBuffer:S.frame[1] offset:0 atIndex:2];
        [enc setBuffer:S.frame[2] offset:0 atIndex:3];
        [enc setBuffer:S.frame[0] offset:0 atIndex:4];
        [enc setBytes:&P length:sizeof P atIndex:5];
        [enc setBuffer:(S.mnodes ? S.mnodes : S.nodes) offset:0 atIndex:6];
        [enc setBuffer:(S.mtris ? S.mtris : S.tris) offset:0 atIndex:7];
        [enc setBuffer:S.counter offset:0 atIndex:8];
        [enc setBuffer:S.attr offset:0 atIndex:9];
        [enc setBuffer:(S.mattr ? S.mattr : S.attr) offset:0 atIndex:10];
        [enc setBuffer:S.texels offset:0 atIndex:11];
        [enc setBuffer:S.texinfo offset:0 atIndex:12];
        [enc setBytes:&light length:sizeof light atIndex:13];
        [enc setBytes:&S.ntex length:4 atIndex:14];
        [enc setTexture:S.m0.tex atIndex:0];
        [enc setTexture:S.m1.tex atIndex:1];
        [enc setTexture:S.hit.tex atIndex:2];
        [enc setTexture:S.hcol.tex atIndex:3];
        [enc setTexture:S.tcol.tex atIndex:4];
        [enc dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            rtlog("rt: Metal command buffer failed: %s", cb.error.localizedDescription.UTF8String);
            return 0;
        }
        if (gpu_ms) *gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
        if (rays) {
            const uint32_t *c = (const uint32_t *)S.counter.contents;
            rays[0] = c[0]; rays[1] = c[1]; rays[2] = c[2]; rays[3] = c[3];
        }
    }
    return S.hit.gl;
}
