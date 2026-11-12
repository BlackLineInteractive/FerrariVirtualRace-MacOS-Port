// Anti-aliasing modes offered in the game's "Anti Aliasing" option.
//
// The game lists the D3D multisample types the device reports (index == type, see the table at
// 0x67a980 in FerrariVR_Hi.exe) and stores the chosen index in a global. We report types 2..9:
//   2 -> FXAA       post pass (FXAA 3.11 style, quality preset ~12)
//   3 -> MSAA 2x    real multisampling of the scene (d3d9.cpp, "MSAA shadows")
//   4 -> MSAA 4x
//   5 -> MSAA 8x
//   6 -> FSR 1      MSAA 4x, then AMD FidelityFX Super Resolution 1: EASU upscale to the window's
//                   native pixel size and RCAS sharpening
//   7 -> Temporal AA       temporal accumulation in the style of FSR 2 at native resolution: the
//   8 -> Temporal 1.5x     camera is jittered every frame, the scene pass writes per-object motion
//   9 -> Temporal 2x       vectors (d3d9.cpp) and post_taa() accumulates the jittered frames into a
//                          history buffer. 8 / 9 render the 3D scene at 1/1.5 and 1/2 of the
//                          resolution and reconstruct the full one (temporal upscaling); the HUD and
//                          post effects stay at full resolution. RCAS sharpening at the end.
#define GL_SILENCE_DEPRECATION
#include "rt.h"
#include <OpenGL/gl3.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>

// Guest global holding the clamped "AntiAlias" index (FerrariVR_Hi.exe).
static const uint32_t kGuestAntiAlias = 0x9cf280;

// post: 0 none, 1 FXAA, 2 FSR 1, 3 temporal (sharpen); scale: temporal render scale (0: not temporal)
struct AaMode { int post, samples; const char *label; float scale; };
static const AaMode kModes[] = {
    {0, 0, nullptr, 0}, {0, 0, nullptr, 0}, {1, 0, "FXAA", 0}, {0, 2, "MSAA 2x", 0}, {0, 4, "MSAA 4x", 0}, {0, 8, "MSAA 8x", 0},
    {2, 4, "FSR 1", 0}, {3, 0, "Temporal AA", 1.0f}, {3, 0, "Temporal 1.5x", 1.0f / 1.5f}, {3, 0, "Temporal 2x", 0.5f},
};
static const int kNumModes = sizeof kModes / sizeof kModes[0];

static const AaMode &aa_mode() {
    static int forced = -2;
    if (forced == -2) {
        const char *e = getenv("FVR_AA");
        forced = -1;
        if (e) {
            forced = 0;
            static const char *names[] = {"off", "", "fxaa", "msaa2", "msaa4", "msaa8", "fsr", "taa", "taa15", "taa2"};
            for (int i = 0; i < kNumModes; i++) if (!strcmp(e, names[i])) forced = i;
        }
    }
    uint32_t v = forced >= 0 ? (uint32_t)forced : R32(kGuestAntiAlias);
    return kModes[v < (uint32_t)kNumModes ? v : 0];
}
int post_aa_mode() { return aa_mode().post; }
int post_msaa_samples() { return aa_mode().samples; }
float post_temporal_scale() {
    static float forced = -1;
    if (forced < 0) { const char *e = getenv("FVR_TAA_SCALE"); forced = e ? (float)atof(e) : 0; }   // debug override
    float s = aa_mode().scale;
    return s > 0 && forced > 0 ? forced : s;
}
bool post_aa_type_supported(uint32_t t) { return t == 0 || (t < (uint32_t)kNumModes && kModes[t].label); }

// Options menu: the game formats each entry as "%dx" from the multisample type.
extern "C" void fvr_hook_aa_label(CPU *c) {
    uint32_t buf = R32(c->esp), n = R32(c->esp + 4), idx = R32(c->esp + 12);
    char tmp[16];
    const char *s = idx < (uint32_t)kNumModes ? kModes[idx].label : nullptr;
    if (!s) { snprintf(tmp, sizeof tmp, "%ux", idx); s = tmp; }
    gstrcpy(buf, s, n);
    c->eax = (uint32_t)strlen(s);
}

// ---------------------------------------------------------------------------------------------
static const char *kVS = R"(#version 410 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Common prologue: rect = output viewport (x, y, w, h); flip = write rows bottom-up.
static const char *kCommon = R"(#version 410 core
uniform sampler2D src;
uniform vec4 rect;
uniform int flip;
layout(location = 0) out vec4 o;
vec2 outPos() {
    vec2 p = gl_FragCoord.xy - rect.xy;
    if (flip != 0) p.y = rect.w - p.y;
    return p;
}
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
)";

static const char *kFXAA = R"(
const float EDGE_MIN = 0.0312, EDGE_MAX = 0.125, SUBPIX = 0.75;
float Q(int i) { return i < 5 ? 1.0 : i == 5 ? 1.5 : i < 10 ? 2.0 : i == 10 ? 4.0 : 8.0; }
vec3 S(vec2 uv) { return textureLod(src, uv, 0.0).rgb; }
void main() {
    vec2 rcp = 1.0 / vec2(textureSize(src, 0));
    vec2 uv = outPos() * rcp;
    vec3 cC = S(uv);
    float lC = lum(cC);
    float lD = lum(textureLodOffset(src, uv, 0.0, ivec2(0, -1)).rgb);
    float lU = lum(textureLodOffset(src, uv, 0.0, ivec2(0, 1)).rgb);
    float lL = lum(textureLodOffset(src, uv, 0.0, ivec2(-1, 0)).rgb);
    float lR = lum(textureLodOffset(src, uv, 0.0, ivec2(1, 0)).rgb);
    float lMin = min(lC, min(min(lD, lU), min(lL, lR)));
    float lMax = max(lC, max(max(lD, lU), max(lL, lR)));
    float range = lMax - lMin;
    if (range < max(EDGE_MIN, lMax * EDGE_MAX)) { o = vec4(cC, 1.0); return; }
    float lDL = lum(textureLodOffset(src, uv, 0.0, ivec2(-1, -1)).rgb);
    float lUR = lum(textureLodOffset(src, uv, 0.0, ivec2(1, 1)).rgb);
    float lUL = lum(textureLodOffset(src, uv, 0.0, ivec2(-1, 1)).rgb);
    float lDR = lum(textureLodOffset(src, uv, 0.0, ivec2(1, -1)).rgb);
    float lDU = lD + lU, lLR = lL + lR;
    float lLC = lDL + lUL, lDC = lDL + lDR, lRC = lDR + lUR, lUC = lUR + lUL;
    float eH = abs(-2.0 * lL + lLC) + abs(-2.0 * lC + lDU) * 2.0 + abs(-2.0 * lR + lRC);
    float eV = abs(-2.0 * lU + lUC) + abs(-2.0 * lC + lLR) * 2.0 + abs(-2.0 * lD + lDC);
    bool hor = eH >= eV;
    float l1 = hor ? lD : lL, l2 = hor ? lU : lR;
    float g1 = l1 - lC, g2 = l2 - lC;
    bool steep1 = abs(g1) >= abs(g2);
    float gScaled = 0.25 * max(abs(g1), abs(g2));
    float stepLen = hor ? rcp.y : rcp.x;
    float lAvgLocal;
    if (steep1) { stepLen = -stepLen; lAvgLocal = 0.5 * (l1 + lC); } else lAvgLocal = 0.5 * (l2 + lC);
    vec2 cur = uv;
    if (hor) cur.y += stepLen * 0.5; else cur.x += stepLen * 0.5;
    vec2 off = hor ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    vec2 uv1 = cur - off, uv2 = cur + off;
    float le1 = lum(S(uv1)) - lAvgLocal, le2 = lum(S(uv2)) - lAvgLocal;
    bool r1 = abs(le1) >= gScaled, r2 = abs(le2) >= gScaled;
    if (!r1) uv1 -= off;
    if (!r2) uv2 += off;
    if (!(r1 && r2)) {
        for (int i = 2; i < 12; i++) {
            if (!r1) le1 = lum(S(uv1)) - lAvgLocal;
            if (!r2) le2 = lum(S(uv2)) - lAvgLocal;
            r1 = abs(le1) >= gScaled; r2 = abs(le2) >= gScaled;
            if (!r1) uv1 -= off * Q(i);
            if (!r2) uv2 += off * Q(i);
            if (r1 && r2) break;
        }
    }
    float d1 = hor ? uv.x - uv1.x : uv.y - uv1.y;
    float d2 = hor ? uv2.x - uv.x : uv2.y - uv.y;
    bool dir1 = d1 < d2;
    float dmin = min(d1, d2), thick = d1 + d2;
    float pixOff = -dmin / thick + 0.5;
    bool cSmaller = lC < lAvgLocal;
    bool good = ((dir1 ? le1 : le2) < 0.0) != cSmaller;
    float fin = good ? pixOff : 0.0;
    float lAvg = (1.0 / 12.0) * (2.0 * (lDU + lLR) + lLC + lRC);
    float s1 = clamp(abs(lAvg - lC) / range, 0.0, 1.0);
    float s2 = (-2.0 * s1 + 3.0) * s1 * s1;
    fin = max(fin, s2 * s2 * SUBPIX);
    vec2 fuv = uv;
    if (hor) fuv.y += fin * stepLen; else fuv.x += fin * stepLen;
    o = vec4(S(fuv), 1.0);
}
)";

// FSR 1 EASU (edge adaptive spatial upsampling), after AMD's reference ffx_fsr1.h (MIT licence).
static const char *kEASU = R"(
vec3 F(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0).rgb; }
float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void easuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float dc = lD - lC, cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0); lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0); lenY *= lenY;
    len += lenY * w;
}
void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w; aW += w;
}
void main() {
    vec2 inSize = vec2(textureSize(src, 0));
    vec2 scale = inSize / rect.zw;
    vec2 pp = floor(outPos()) * scale + (0.5 * scale - 0.5);
    vec2 fp = floor(pp); pp -= fp;
    ivec2 f0 = ivec2(fp);
    vec3 b = F(f0 + ivec2(0, -1)), c = F(f0 + ivec2(1, -1));
    vec3 e = F(f0 + ivec2(-1, 0)), f = F(f0), g = F(f0 + ivec2(1, 0)), h = F(f0 + ivec2(2, 0));
    vec3 i = F(f0 + ivec2(-1, 1)), j = F(f0 + ivec2(0, 1)), k = F(f0 + ivec2(1, 1)), l = F(f0 + ivec2(2, 1));
    vec3 n = F(f0 + ivec2(0, 2)), oo = F(f0 + ivec2(1, 2));
    float bL = L(b), cL = L(c), eL = L(e), fL = L(f), gL = L(g), hL = L(h);
    float iL = L(i), jL = L(j), kL = L(k), lL = L(l), nL = L(n), oL = L(oo);
    vec2 dir = vec2(0.0); float len = 0.0;
    easuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5; len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0); float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oo);
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    o = vec4(min(mx, max(mn, aC / aW)), 1.0);
}
)";

// FSR 1 RCAS (robust contrast adaptive sharpening).
static const char *kRCAS = R"(
uniform float sharp;   // exp2(-stops)
vec3 F(ivec2 p) { return texelFetch(src, clamp(p, ivec2(0), textureSize(src, 0) - 1), 0).rgb; }
void main() {
    ivec2 p = ivec2(outPos());
    vec3 b = F(p + ivec2(0, -1)), d = F(p + ivec2(-1, 0)), e = F(p), f = F(p + ivec2(1, 0)), h = F(p + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
    vec2 peak = vec2(1.0, -4.0);
    vec3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);
    vec3 hitMax = (peak.x - max(mx4, e)) / (4.0 * mn4 + peak.y);
    vec3 lobeC = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeC.r, max(lobeC.g, lobeC.b)), 0.0)) * sharp;
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g), dL = d.b * 0.5 + (d.r * 0.5 + d.g), eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g), hL = h.b * 0.5 + (h.r * 0.5 + h.g);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = clamp(abs(nz) / max(rng, 1e-5), 0.0, 1.0);
    lobe *= -0.5 * nz + 1.0;
    o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);
}
)";

// Plain resampling copy (bilinear) used when the output is not larger than the input.
static const char *kCopy = R"(
void main() { o = vec4(textureLod(src, outPos() / rect.zw, 0.0).rgb, 1.0); }
)";

struct Pass { GLuint prog = 0; GLint src = -1, rect = -1, flip = -1, sharp = -1; };
static Pass g_fxaa, g_easu, g_rcas, g_copy;
static GLuint g_vao, g_sampler;
static bool g_init, g_broken;

struct Target { GLuint tex = 0, fbo = 0; int w = 0, h = 0; };
static Target g_a, g_b;

static GLuint compile(GLenum type, const std::string &src) {
    GLuint s = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, nullptr, log); rtlog("post: shader error: %s", log); glDeleteShader(s); return 0; }
    return s;
}
static bool build(Pass &p, const char *body) {
    GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, std::string(kCommon) + body);
    if (!vs || !fs) return false;
    p.prog = glCreateProgram();
    glAttachShader(p.prog, vs); glAttachShader(p.prog, fs);
    glLinkProgram(p.prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok) { rtlog("post: link failed"); return false; }
    p.src = glGetUniformLocation(p.prog, "src"); p.rect = glGetUniformLocation(p.prog, "rect");
    p.flip = glGetUniformLocation(p.prog, "flip"); p.sharp = glGetUniformLocation(p.prog, "sharp");
    return true;
}
static bool init() {
    if (g_init) return !g_broken;
    g_init = true;
    if (!build(g_fxaa, kFXAA) || !build(g_easu, kEASU) || !build(g_rcas, kRCAS) || !build(g_copy, kCopy)) { g_broken = true; return false; }
    glGenVertexArrays(1, &g_vao);
    glGenSamplers(1, &g_sampler);
    glSamplerParameteri(g_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    rtlog("post: FXAA / FSR passes ready");
    return true;
}
static void ensure(Target &t, int w, int h) {
    if (t.tex && t.w == w && t.h == h) return;
    if (!t.tex) { glGenTextures(1, &t.tex); glGenFramebuffers(1, &t.fbo); }
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    t.w = w; t.h = h;
}
// Draw one pass from texture src into the bound draw framebuffer at (x, y, w, h).
static void run(const Pass &p, GLuint src, int x, int y, int w, int h, bool flip, float sharp = 0.0f) {
    glViewport(x, y, w, h);
    glUseProgram(p.prog);
    glUniform1i(p.src, 0);
    glUniform4f(p.rect, (float)x, (float)y, (float)w, (float)h);
    glUniform1i(p.flip, flip ? 1 : 0);
    if (p.sharp >= 0) glUniform1f(p.sharp, sharp);
    glBindTexture(GL_TEXTURE_2D, src);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// Present the back buffer texture `bb` (bw x bh, top row first) into window framebuffer 0 at
// (dx, dy, dw, dh). Returns false when nothing was drawn (mode off or setup failure).
bool post_present(int mode, GLuint bb, int bw, int bh, int dx, int dy, int dw, int dh) {
    if (!mode || !bb || !init()) return false;
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_FRAMEBUFFER_SRGB);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, 1, 1, 1, 1);
    glBindVertexArray(g_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, g_sampler);

    if (mode == 1) {
        ensure(g_a, bw, bh);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_a.fbo);
        run(g_fxaa, bb, 0, 0, bw, bh, false);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        run(g_copy, g_a.tex, dx, dy, dw, dh, true);
    } else if (dw > bw || dh > bh) {
        ensure(g_b, dw, dh);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_b.fbo);
        run(g_easu, bb, 0, 0, dw, dh, false);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        run(g_rcas, g_b.tex, dx, dy, dw, dh, true, 0.87f);   // exp2(-0.2 stops)
    } else {
        ensure(g_b, bw, bh);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_b.fbo);
        run(g_rcas, bb, 0, 0, bw, bh, false, 0.87f);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        run(g_copy, g_b.tex, dx, dy, dw, dh, true);
    }
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Copy a single-sampled colour (and optionally depth) texture into the bound multisampled draw
// framebuffer, writing every sample. Used when the game wrote a render target outside the
// multisampled path and then draws into it again. The caller restores its GL state cache.
static const char *kFillFS = R"(#version 410 core
uniform sampler2D col;
uniform sampler2D dep;
uniform int mode;   // bit 0: colour, bit 1: depth
layout(location = 0) out vec4 o;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    o = (mode & 1) != 0 ? texelFetch(col, p, 0) : vec4(0.0);
    gl_FragDepth = (mode & 2) != 0 ? texelFetch(dep, p, 0).r : 1.0;
}
)";
void post_fill_ms(GLuint color, GLuint depth, int w, int h) {
    static GLuint prog = 0, vao = 0; static GLint lcol, ldep, lmode;
    if (!prog) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(vs, 1, &kVS, nullptr); glCompileShader(vs);
        glShaderSource(fs, 1, &kFillFS, nullptr); glCompileShader(fs);
        prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
        glDeleteShader(vs); glDeleteShader(fs);
        lcol = glGetUniformLocation(prog, "col"); ldep = glGetUniformLocation(prog, "dep"); lmode = glGetUniformLocation(prog, "mode");
        glGenVertexArrays(1, &vao);
    }
    glDisable(GL_BLEND); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_FRAMEBUFFER_SRGB); glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(color ? 1 : 0, color ? 1 : 0, color ? 1 : 0, color ? 1 : 0);
    if (depth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE); } else glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);
    glDepthRange(0.0, 1.0);
    glUseProgram(prog);
    glUniform1i(lcol, 0); glUniform1i(ldep, 1);
    glUniform1i(lmode, (color ? 1 : 0) | (depth ? 2 : 0));
    glActiveTexture(GL_TEXTURE0); glBindSampler(0, 0); glBindTexture(GL_TEXTURE_2D, color);
    glActiveTexture(GL_TEXTURE1); glBindSampler(1, 0); glBindTexture(GL_TEXTURE_2D, depth);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------------------------
// Temporal accumulation / upscaling (FSR 2 style). Inputs at render resolution: HDR colour, depth
// and motion vectors (texture-space offset since last frame, z = 1 where unknown); the camera was
// offset by `jit` pixels this frame. The history lives at output resolution in a tone-mapped space
// with the accumulated sample weight in alpha.
static const char *kTaaFS = R"(#version 410 core
uniform sampler2D col, dep, vel, hist;
uniform vec2 lowSize, outSize, jit;
uniform float maxW;
uniform int reset, dbg;
layout(location = 0) out vec4 oHist;
layout(location = 1) out vec4 oDst;
vec3 tm(vec3 c) { return c / (1.0 + max(max(c.r, c.g), c.b)); }
vec3 itm(vec3 c) { return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-4); }
vec3 ycc(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 rgb(vec3 y) { return vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }
vec4 H(vec2 uv) { return textureLod(hist, uv, 0.0); }
// Catmull-Rom history sample from 5 bilinear taps
vec4 histCR(vec2 uv) {
    vec2 p = uv * outSize, t1 = floor(p - 0.5) + 0.5, f = p - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (t1 - 1.0) / outSize, t3 = (t1 + 2.0) / outSize, t12 = (t1 + w2 / w12) / outSize;
    vec4 r = H(vec2(t12.x, t0.y)) * (w12.x * w0.y) + H(vec2(t0.x, t12.y)) * (w0.x * w12.y) + H(t12) * (w12.x * w12.y)
           + H(vec2(t3.x, t12.y)) * (w3.x * w12.y) + H(vec2(t12.x, t3.y)) * (w12.x * w3.y);
    float ws = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / ws, vec4(0.0));
}
// one neighbourhood sample: reconstruction weight by its distance (output pixels), colour moments,
// nearest depth for the motion vector
void tap(ivec2 i0, ivec2 o, vec2 lp, vec2 ratio, ivec2 mx, inout vec3 sum, inout float sw, inout vec3 m1, inout vec3 m2,
         inout float zmin, inout ivec2 best, inout float alpha) {
    ivec2 q = clamp(i0 + o, ivec2(0), mx);
    vec4 cs = texelFetch(col, q, 0);
    vec3 c = tm(max(cs.rgb, vec3(0.0)));
    vec2 d = (vec2(i0 + o) + 0.5 - jit - lp) * ratio;
    float x = clamp(1.0 - 0.4 * dot(d, d), 0.0, 1.0);   // ~exp(-2.3 d^2) near the centre
    float w = x * x;
    sum += c * w; sw += w;
    vec3 yc = ycc(c); m1 += yc; m2 += yc * yc;
    if (o == ivec2(0)) alpha = cs.a;
    float z = texelFetch(dep, q, 0).r;
    if (z < zmin) { zmin = z; best = q; }
}
void main() {
    vec2 uv = gl_FragCoord.xy / outSize;
    vec2 lp = uv * lowSize;                 // output pixel centre in render pixels
    vec2 ratio = outSize / lowSize;         // output pixels per render pixel
    ivec2 i0 = ivec2(floor(lp + jit)), mx = ivec2(lowSize) - 1;
    vec3 sum = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0);
    float sw = 0.0, zmin = 2.0, alpha = 1.0;
    ivec2 best = clamp(i0, ivec2(0), mx);
    tap(i0, ivec2(0, 0), lp, ratio, mx, sum, sw, m1, m2, zmin, best, alpha);
    tap(i0, ivec2(-1, 0), lp, ratio, mx, sum, sw, m1, m2, zmin, best, alpha);
    tap(i0, ivec2(1, 0), lp, ratio, mx, sum, sw, m1, m2, zmin, best, alpha);
    tap(i0, ivec2(0, -1), lp, ratio, mx, sum, sw, m1, m2, zmin, best, alpha);
    tap(i0, ivec2(0, 1), lp, ratio, mx, sum, sw, m1, m2, zmin, best, alpha);
    vec3 cur = sum / max(sw, 1e-6);
    vec3 res = cur;
    float w = min(sw, 1.0);
    vec4 v = texelFetch(vel, best, 0);      // motion of the nearest surface around this pixel
    vec2 huv = uv - v.xy;
    if (dbg == 2) { oHist = vec4(cur, 1.0); oDst = vec4(itm(cur), alpha); return; }
    if (reset == 0 && v.z < 0.5 && all(greaterThanEqual(huv, vec2(0.0))) && all(lessThanEqual(huv, vec2(1.0)))) {
        vec4 h = histCR(huv);
        vec3 mean = m1 / 5.0, sig = sqrt(max(m2 / 5.0 - mean * mean, 0.0));
        vec3 hy = ycc(h.rgb), dv = hy - mean, u = abs(dv) / (sig * 1.25 + 1e-4);
        float m = max(u.x, max(u.y, u.z));
        vec3 hc = m > 1.0 ? mean + dv / m : hy;    // clip the history into the neighbourhood's colour box
        float hw = h.a / max(m, 1.0);              // and trust it less the further it was off
        float a = w / (hw + w);
        res = mix(rgb(hc), cur, a);
        w = min(hw + w, maxW);
    }
    oHist = vec4(res, w);
    oDst = vec4(itm(res), alpha);
}
)";
static Target g_hist[2];   // RGBA16F history, ping-pong
static int g_hist_cur;
static GLuint g_taa_prog, g_taa_fbo[2];
static GLint g_taa_u[11];

void post_taa_jitter(uint64_t frame, float scale, float *jx, float *jy) {
    int phases = (int)ceilf(8.0f / (scale * scale));
    uint32_t i = (uint32_t)(frame % phases) + 1;
    auto halton = [](uint32_t i, uint32_t b) { float f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; };
    *jx = halton(i, 2) - 0.5f;
    *jy = halton(i, 3) - 0.5f;
}

bool post_taa(GLuint col, GLuint dep, GLuint vel, int lw, int lh, GLuint dst, int ow, int oh, float jx, float jy, bool reset) {
    if (!init()) return false;
    if (!g_taa_prog) {
        GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kTaaFS);
        if (!vs || !fs) { g_broken = true; return false; }
        g_taa_prog = glCreateProgram();
        glAttachShader(g_taa_prog, vs); glAttachShader(g_taa_prog, fs); glLinkProgram(g_taa_prog);
        glDeleteShader(vs); glDeleteShader(fs);
        const char *n[] = {"col", "dep", "vel", "hist", "lowSize", "outSize", "jit", "maxW", "reset", "dbg"};
        for (int i = 0; i < 10; i++) g_taa_u[i] = glGetUniformLocation(g_taa_prog, n[i]);
        glGenFramebuffers(2, g_taa_fbo);
        rtlog("post: temporal AA pass ready");
    }
    for (int i = 0; i < 2; i++) {
        Target &t = g_hist[i];
        if (t.tex && t.w == ow && t.h == oh) continue;
        if (!t.tex) glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, ow, oh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        t.w = ow; t.h = oh;
        reset = true;
    }
    int nx = g_hist_cur ^ 1;
    glBindFramebuffer(GL_FRAMEBUFFER, g_taa_fbo[nx]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_hist[nx].tex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, dst, 0);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_FRAMEBUFFER_SRGB);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, 1, 1, 1, 1); glColorMaski(1, 1, 1, 1, 1);
    glViewport(0, 0, ow, oh);
    glDepthRange(0.0, 1.0);
    glUseProgram(g_taa_prog);
    GLuint tex[4] = {col, dep, vel, g_hist[g_hist_cur].tex};
    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindSampler(i, i == 3 ? g_sampler : 0);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glUniform1i(g_taa_u[i], i);
    }
    float scale = (float)lw / ow;
    glUniform2f(g_taa_u[4], (float)lw, (float)lh);
    glUniform2f(g_taa_u[5], (float)ow, (float)oh);
    glUniform2f(g_taa_u[6], jx, jy);
    glUniform1f(g_taa_u[7], scale > 0.9f ? 10.0f : scale > 0.6f ? 16.0f : 24.0f);
    glUniform1i(g_taa_u[8], reset ? 1 : 0);
    static int dbg = getenv("FVR_TAA_DBG") ? atoi(getenv("FVR_TAA_DBG")) : 0;
    glUniform1i(g_taa_u[9], dbg);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    for (int i = 3; i >= 0; i--) { glActiveTexture(GL_TEXTURE0 + i); glBindSampler(i, 0); glBindTexture(GL_TEXTURE_2D, 0); }
    g_hist_cur = nx;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Screen-space reflections on the track surface, at render resolution before temporal
// accumulation (which also averages the per-frame ray jitter). Positions come from the depth
// buffer and the camera's view-projection (ViewProjMtx, rows = clip components), normals from
// neighbouring depths. Only near-horizontal, unsaturated surfaces (asphalt, kerb paint, the cars'
// roofs) reflect; grass is excluded by its saturation. The game has no material information to
// go by, so this is a heuristic.
static const char *kSsrFS = R"(#version 410 core
uniform sampler2D col, dep;
uniform mat4 VP, IVP;
uniform vec3 cam;
uniform vec2 size;
uniform float strength, seed;
uniform vec2 zlin;    // depth -> view distance along the axis: w = zlin.y / (z - zlin.x)
uniform int dbg;
layout(location = 0) out vec4 o;
vec3 wpos(vec2 uv, float z) { vec4 p = IVP * vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, z, 1.0); return p.xyz / p.w; }
float D(ivec2 p) { return texelFetch(dep, clamp(p, ivec2(0), ivec2(size) - 1), 0).r; }
vec3 P(ivec2 p) { return wpos((vec2(p) + 0.5) / size, D(p)); }
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233)) + seed) * 43758.5453); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(col, p, 0);
    o = c;
    float z = D(p);
    if (z >= 0.99999) return;
    vec3 X = P(p);
    float zl = D(p - ivec2(1, 0)), zr = D(p + ivec2(1, 0)), zd = D(p - ivec2(0, 1)), zu = D(p + ivec2(0, 1));
    vec3 dx = abs(zr - z) < abs(z - zl) ? P(p + ivec2(1, 0)) - X : X - P(p - ivec2(1, 0));
    vec3 dy = abs(zu - z) < abs(z - zd) ? P(p + ivec2(0, 1)) - X : X - P(p - ivec2(0, 1));
    vec3 N = normalize(cross(dx, dy));
    vec3 V = X - cam;
    float dist = length(V);
    V /= dist;
    if (dot(N, V) > 0.0) N = -N;
    if (dbg == 1) { o.rgb = N * 0.5 + 0.5; return; }
    if (N.y < 0.9) return;                       // near-horizontal surfaces only
    float mx = max(c.r, max(c.g, c.b));
    float mask = 1.0 - smoothstep(0.04, 0.15, (c.g - max(c.r, c.b)) / (mx + 1e-4));   // not grass
    float cosv = max(dot(-V, N), 0.0);
    float F = (0.04 + 0.96 * pow(1.0 - cosv, 5.0)) * strength * mask;
    if (dbg == 2) { o.rgb = vec3(F, mask, 0.0); return; }
    if (F < 0.005) return;
    // roughness: jitter the normal a little each frame
    float r1 = hash(gl_FragCoord.xy), r2 = hash(gl_FragCoord.yx + 7.0);
    N = normalize(N + 0.04 * vec3(r1 - 0.5, 0.0, r2 - 0.5));
    vec3 R = reflect(V, N);
    float t = dist * 0.02 * (0.5 + r1), tmax = dist * 4.0;
    vec2 hit = vec2(-1.0), sky = vec2(-1.0);
    float prevT = 0.0;
    for (int i = 0; i < 16 && t < tmax; i++) {
        vec3 Q = X + R * t;
        vec4 q = VP * vec4(Q, 1.0);
        if (q.w <= 0.0) break;
        vec3 n = q.xyz / q.w;
        vec2 suv = vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y);
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
        float sz = texture(dep, suv).r;
        if (sz >= 0.99999) sky = suv;               // passing over the sky: reflect it if nothing is hit
        if (n.z > sz && sz < 0.99999) {
            float behind = q.w - zlin.y / (sz - zlin.x);
            if (behind < t * 0.15 + 0.5) {          // thin enough to be the surface we went behind
                // refine between the last two steps
                float a = prevT, b = t;
                for (int k = 0; k < 4; k++) {
                    float m = 0.5 * (a + b);
                    vec4 qm = VP * vec4(X + R * m, 1.0);
                    vec3 nm = qm.xyz / qm.w;
                    vec2 um = vec2(nm.x * 0.5 + 0.5, 0.5 - 0.5 * nm.y);
                    if (nm.z > texture(dep, um).r) { b = m; suv = um; } else a = m;
                }
                hit = suv;
            }
            break;
        }
        prevT = t;
        t *= 1.35;
    }
    float wgt = 1.0;
    if (hit.x < 0.0 && sky.x >= 0.0) { hit = sky; wgt = 0.6; }
    if (dbg == 3) { o.rgb = vec3(F * 4.0, hit.x >= 0.0 ? 1.0 : 0.0, 0.0); return; }
    if (hit.x < 0.0) return;
    vec2 e = min(hit, 1.0 - hit);
    float fade = smoothstep(0.0, 0.08, min(e.x, e.y));
    vec3 rc = textureLod(col, hit, 0.0).rgb;
    o.rgb = mix(c.rgb, rc, F * fade * wgt);
}
)";
static GLuint g_ssr_prog, g_ssr_fbo;
static GLint g_ssr_u[10];
static Target g_ssr;

// Returns the texture holding colour with reflections (or col on failure).
GLuint post_ssr(GLuint col, GLuint dep, int w, int h, const float *vp, const float *ivp, const float *cam, float strength, uint64_t frame) {
    if (!init()) return col;
    if (!g_ssr_prog) {
        GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kSsrFS);
        if (!vs || !fs) return col;
        g_ssr_prog = glCreateProgram();
        glAttachShader(g_ssr_prog, vs); glAttachShader(g_ssr_prog, fs); glLinkProgram(g_ssr_prog);
        glDeleteShader(vs); glDeleteShader(fs);
        const char *n[] = {"col", "dep", "VP", "IVP", "cam", "size", "strength", "seed", "dbg", "zlin"};
        for (int i = 0; i < 10; i++) g_ssr_u[i] = glGetUniformLocation(g_ssr_prog, n[i]);
        glGenFramebuffers(1, &g_ssr_fbo);
        rtlog("post: screen-space reflections ready");
    }
    if (!g_ssr.tex || g_ssr.w != w || g_ssr.h != h) {
        if (!g_ssr.tex) glGenTextures(1, &g_ssr.tex);
        glBindTexture(GL_TEXTURE_2D, g_ssr.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ssr_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ssr.tex, 0);
        g_ssr.w = w; g_ssr.h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_ssr_fbo);
    GLenum b0 = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &b0);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_FRAMEBUFFER_SRGB);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, 1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glUseProgram(g_ssr_prog);
    glActiveTexture(GL_TEXTURE0); glBindSampler(0, g_sampler); glBindTexture(GL_TEXTURE_2D, col); glUniform1i(g_ssr_u[0], 0);
    glActiveTexture(GL_TEXTURE1); glBindSampler(1, 0); glBindTexture(GL_TEXTURE_2D, dep); glUniform1i(g_ssr_u[1], 1);
    glUniformMatrix4fv(g_ssr_u[2], 1, GL_TRUE, vp);
    glUniformMatrix4fv(g_ssr_u[3], 1, GL_TRUE, ivp);
    glUniform3fv(g_ssr_u[4], 1, cam);
    glUniform2f(g_ssr_u[5], (float)w, (float)h);
    glUniform1f(g_ssr_u[6], strength);
    glUniform1f(g_ssr_u[7], (float)(frame % 64) * 1.618f);
    static int dbg = getenv("FVR_SSR_DEBUG") ? atoi(getenv("FVR_SSR_DEBUG")) : 0;
    glUniform1i(g_ssr_u[8], dbg);
    // perspective projection: clip.z = A * clip.w + B (rows 2 and 3 of VP)
    int k = 0;
    for (int i = 1; i < 3; i++) if (fabsf(vp[12 + i]) > fabsf(vp[12 + k])) k = i;
    float A = vp[12 + k] != 0 ? vp[8 + k] / vp[12 + k] : 1.0f, B = vp[11] - A * vp[15];
    glUniform2f(g_ssr_u[9], A, B);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindSampler(0, 0); glBindTexture(GL_TEXTURE_2D, 0);
    return g_ssr.tex;
}

// ---------------------------------------------------------------------------------------------
// Ground-truth ambient occlusion (GTAO, Jimenez et al. 2016) at render resolution, before the
// reflections and temporal accumulation (which averages the per-frame slice rotation). Positions
// come from depth and the inverse view-projection (world space; the game's units are well below a
// metre, the default radius of 25 is about a car's height), the normal from
// neighbouring depths. The game hands over finished lighting, so occlusion darkens the final
// colour (mostly ambient-lit areas: under cars, between grandstand seats, at wall bases).
static const char *kAoFS = R"(#version 410 core
uniform sampler2D col, dep;
uniform mat4 IVP;
uniform vec3 cam;
uniform vec2 size;
uniform float strength, radius, projScale, seed;
uniform int dbg;
layout(location = 0) out vec4 o;
const float PI = 3.14159265;
vec3 wpos(vec2 uv, float z) { vec4 p = IVP * vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, z, 1.0); return p.xyz / p.w; }
float D(ivec2 p) { return texelFetch(dep, clamp(p, ivec2(0), ivec2(size) - 1), 0).r; }
vec3 P(ivec2 p) { return wpos((vec2(p) + 0.5) / size, D(p)); }
vec3 Pf(vec2 px) { return wpos(px / size, textureLod(dep, px / size, 0.0).r); }
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(col, p, 0);
    o = c;
    float z = D(p);
    if (z >= 0.99999) return;
    vec3 X = P(p);
    float zl = D(p - ivec2(1, 0)), zr = D(p + ivec2(1, 0)), zd = D(p - ivec2(0, 1)), zu = D(p + ivec2(0, 1));
    vec3 dx = abs(zr - z) < abs(z - zl) ? P(p + ivec2(1, 0)) - X : X - P(p - ivec2(1, 0));
    vec3 dy = abs(zu - z) < abs(z - zd) ? P(p + ivec2(0, 1)) - X : X - P(p - ivec2(0, 1));
    vec3 N = normalize(cross(dx, dy));
    vec3 V = cam - X;
    float dist = length(V);
    V /= dist;
    if (dot(N, V) < 0.0) N = -N;
    float rpx = min(radius * projScale / dist, 48.0);   // radius on screen, pixels
    if (rpx < 1.5) return;
    // interleaved 4x4 noise, rotated every frame
    int ix = p.x & 3, iy = p.y & 3;
    float n1 = fract(float(ix * 4 + iy) * 0.0625 * 7.0 + seed);
    float n2 = fract(float(iy * 4 + ix) * 0.0625 * 5.0 + seed * 1.7);
    const int SLICES = 2, STEPS = 4;
    float vis = 0.0;
    for (int s = 0; s < SLICES; s++) {
        float phi = (float(s) + n1) * PI / float(SLICES);
        vec2 dir = vec2(cos(phi), sin(phi));
        // world-space slice tangent, orthogonal to the view direction
        vec3 T = Pf(gl_FragCoord.xy + dir * 2.0) - X;
        T = normalize(T - V * dot(T, V));
        vec3 ax = normalize(cross(T, V));
        vec3 Np = N - ax * dot(N, ax);
        float lp = length(Np);
        float ng = sign(dot(Np, T)) * acos(clamp(dot(Np / max(lp, 1e-4), V), -1.0, 1.0));
        float c1 = -1.0, c2 = -1.0;
        for (int i = 0; i < STEPS; i++) {
            float t = (float(i) + n2) / float(STEPS);
            vec2 off = dir * max(t * t * rpx, 1.0 + float(i));
            vec3 d2 = Pf(gl_FragCoord.xy + off) - X, d1 = Pf(gl_FragCoord.xy - off) - X;
            float l2 = length(d2), l1 = length(d1);
            float f2 = clamp(1.0 - l2 * l2 / (radius * radius), 0.0, 1.0), f1 = clamp(1.0 - l1 * l1 / (radius * radius), 0.0, 1.0);
            c2 = max(c2, mix(-1.0, dot(d2, V) / max(l2, 1e-4), f2));
            c1 = max(c1, mix(-1.0, dot(d1, V) / max(l1, 1e-4), f1));
        }
        float h1 = -acos(clamp(c1, -1.0, 1.0)), h2 = acos(clamp(c2, -1.0, 1.0));
        h1 = ng + max(h1 - ng, -PI * 0.5);
        h2 = ng + min(h2 - ng, PI * 0.5);
        float sn = sin(ng), cn = cos(ng);
        vis += lp * 0.25 * ((-cos(2.0 * h1 - ng) + cn + 2.0 * h1 * sn) + (-cos(2.0 * h2 - ng) + cn + 2.0 * h2 * sn));
    }
    float ao = clamp(vis / float(SLICES), 0.0, 1.0);
    if (dbg == 4) { o.rgb = vec3(ao); return; }
    // grass is alpha-tested cards: full occlusion between them turns it black and noisy
    float mx = max(c.r, max(c.g, c.b));
    float grass = smoothstep(0.04, 0.15, (c.g - max(c.r, c.b)) / (mx + 1e-4));
    o.rgb = c.rgb * mix(1.0, ao, strength * (1.0 - 0.6 * grass));
}
)";
static GLuint g_ao_prog, g_ao_fbo;
static GLint g_ao_u[10];
static Target g_ao;

GLuint post_ao(GLuint col, GLuint dep, int w, int h, const float *vp, const float *ivp, const float *cam, float strength, uint64_t frame) {
    if (!init()) return col;
    if (!g_ao_prog) {
        GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kAoFS);
        if (!vs || !fs) return col;
        g_ao_prog = glCreateProgram();
        glAttachShader(g_ao_prog, vs); glAttachShader(g_ao_prog, fs); glLinkProgram(g_ao_prog);
        glDeleteShader(vs); glDeleteShader(fs);
        const char *n[] = {"col", "dep", "IVP", "cam", "size", "strength", "radius", "projScale", "seed", "dbg"};
        for (int i = 0; i < 10; i++) g_ao_u[i] = glGetUniformLocation(g_ao_prog, n[i]);
        glGenFramebuffers(1, &g_ao_fbo);
        rtlog("post: ambient occlusion (GTAO) ready");
    }
    if (!g_ao.tex || g_ao.w != w || g_ao.h != h) {
        if (!g_ao.tex) glGenTextures(1, &g_ao.tex);
        glBindTexture(GL_TEXTURE_2D, g_ao.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ao_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ao.tex, 0);
        g_ao.w = w; g_ao.h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_ao_fbo);
    GLenum b0 = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &b0);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_FRAMEBUFFER_SRGB);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, 1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glUseProgram(g_ao_prog);
    glActiveTexture(GL_TEXTURE0); glBindSampler(0, 0); glBindTexture(GL_TEXTURE_2D, col); glUniform1i(g_ao_u[0], 0);
    glActiveTexture(GL_TEXTURE1); glBindSampler(1, 0); glBindTexture(GL_TEXTURE_2D, dep); glUniform1i(g_ao_u[1], 1);
    glUniformMatrix4fv(g_ao_u[2], 1, GL_TRUE, ivp);
    glUniform3fv(g_ao_u[3], 1, cam);
    glUniform2f(g_ao_u[4], (float)w, (float)h);
    glUniform1f(g_ao_u[5], strength);
    static float radius = getenv("FVR_AO_RADIUS") ? (float)atof(getenv("FVR_AO_RADIUS")) : 25.0f;   // world units
    glUniform1f(g_ao_u[6], radius);
    // pixels per world unit at distance 1: half the height times the projection's y scale
    float py = sqrtf(vp[4] * vp[4] + vp[5] * vp[5] + vp[6] * vp[6]);
    glUniform1f(g_ao_u[7], 0.5f * h * py);
    glUniform1f(g_ao_u[8], (float)(frame % 16) * 0.618f);
    static int dbg = getenv("FVR_SSR_DEBUG") ? atoi(getenv("FVR_SSR_DEBUG")) : 0;
    glUniform1i(g_ao_u[9], dbg);
    glBindVertexArray(g_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    return g_ao.tex;
}

// ---------------------------------------------------------------------------------------------
// Ray-traced reflections (rt=1): Metal traced one ray for each pixel the PBR car material marked
// (m0 = direction, m1 = position - camera and weight) and left the hit points in `hit`. The colour
// at a hit is the frame's own colour where the hit point is visible on screen (exact reflection,
// with parallax and contact), otherwise the game's reflection cube in that direction (also for
// misses: the sky). Added to the scene colour with the material's weight.
static const char *kRtFS = R"(#version 410 core
uniform sampler2D col, dep;
uniform sampler2DRect m0, m1, hit, hcol;
uniform samplerCube env;
uniform int haveEnv, dbg;
uniform mat4 VP, IVP;
uniform vec3 cam;
layout(location = 0) out vec4 o;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(col, p, 0);
    o = c;
    vec4 b = texelFetch(m1, p);
    if (!(b.w > 0.0)) { if (dbg != 0) o.rgb *= 0.25; return; }
    vec3 R = normalize(texelFetch(m0, p).xyz);
    vec4 h = texelFetch(hit, p);
    vec3 L = haveEnv != 0 ? textureLod(env, R, 0.0).rgb : vec3(0.0);
    int kind = 0;                                  // 0 cube (miss), 1 screen, 2 shaded by Metal (off screen)
    if (h.w > 0.5 && h.w < 1.5) {
        L = texelFetch(hcol, p).rgb; kind = 2;
        vec4 q = VP * vec4(h.xyz, 1.0);
        if (q.w > 0.0) {
            vec3 n = q.xyz / q.w;
            vec2 uv = vec2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y);
            if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
                float sz = textureLod(dep, uv, 0.0).r;
                vec4 sp = IVP * vec4(n.xy, sz, 1.0);
                vec3 S = sp.xyz / sp.w;
                float d = length(h.xyz - cam);
                if (length(S - h.xyz) < 0.02 * d + 0.05) { L = textureLod(col, uv, 0.0).rgb; kind = 1; }
            }
        }
    }
    if (dbg == 1) { o.rgb = kind == 1 ? vec3(0.0, 1.0, 0.0) : (kind == 2 ? vec3(1.0, 0.2, 0.0) : vec3(0.0, 0.3, 1.0)); return; }
    if (dbg == 2) { o.rgb = L; return; }
    o.rgb = c.rgb + b.w * L;
}
)";
static GLuint g_rt_prog, g_rt_fbo;
static GLint g_rt_u[12];
static Target g_rt;

GLuint post_rt(GLuint col, GLuint dep, int w, int h, GLuint m0, GLuint m1, GLuint hit, GLuint hcol, GLuint cube, const float *vp, const float *ivp, const float *cam) {
    if (!init()) return col;
    if (!g_rt_prog) {
        GLuint vs = compile(GL_VERTEX_SHADER, kVS), fs = compile(GL_FRAGMENT_SHADER, kRtFS);
        if (!vs || !fs) return col;
        g_rt_prog = glCreateProgram();
        glAttachShader(g_rt_prog, vs); glAttachShader(g_rt_prog, fs); glLinkProgram(g_rt_prog);
        glDeleteShader(vs); glDeleteShader(fs);
        const char *n[] = {"col", "dep", "m0", "m1", "hit", "env", "haveEnv", "dbg", "VP", "IVP", "cam", "hcol"};
        for (int i = 0; i < 12; i++) g_rt_u[i] = glGetUniformLocation(g_rt_prog, n[i]);
        glGenFramebuffers(1, &g_rt_fbo);
        rtlog("post: ray-traced reflections ready");
    }
    if (!g_rt.tex || g_rt.w != w || g_rt.h != h) {
        if (!g_rt.tex) glGenTextures(1, &g_rt.tex);
        glBindTexture(GL_TEXTURE_2D, g_rt.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rt_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rt.tex, 0);
        g_rt.w = w; g_rt.h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt_fbo);
    GLenum b0 = GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &b0);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_FRAMEBUFFER_SRGB);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMaski(0, 1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glUseProgram(g_rt_prog);
    glBindVertexArray(g_vao);
    glActiveTexture(GL_TEXTURE0); glBindSampler(0, g_sampler); glBindTexture(GL_TEXTURE_2D, col); glUniform1i(g_rt_u[0], 0);
    glActiveTexture(GL_TEXTURE1); glBindSampler(1, 0); glBindTexture(GL_TEXTURE_2D, dep); glUniform1i(g_rt_u[1], 1);
    glActiveTexture(GL_TEXTURE2); glBindSampler(2, 0); glBindTexture(GL_TEXTURE_RECTANGLE, m0); glUniform1i(g_rt_u[2], 2);
    glActiveTexture(GL_TEXTURE3); glBindSampler(3, 0); glBindTexture(GL_TEXTURE_RECTANGLE, m1); glUniform1i(g_rt_u[3], 3);
    glActiveTexture(GL_TEXTURE4); glBindSampler(4, 0); glBindTexture(GL_TEXTURE_RECTANGLE, hit); glUniform1i(g_rt_u[4], 4);
    glActiveTexture(GL_TEXTURE5); glBindSampler(5, g_sampler); glBindTexture(GL_TEXTURE_CUBE_MAP, cube); glUniform1i(g_rt_u[5], 5);
    glActiveTexture(GL_TEXTURE6); glBindSampler(6, 0); glBindTexture(GL_TEXTURE_RECTANGLE, hcol); glUniform1i(g_rt_u[11], 6);
    glUniform1i(g_rt_u[6], cube ? 1 : 0);
    static int dbg = getenv("FVR_RT_DEBUG") ? atoi(getenv("FVR_RT_DEBUG")) : 0;
    glUniform1i(g_rt_u[7], dbg);
    glUniformMatrix4fv(g_rt_u[8], 1, GL_TRUE, vp);
    glUniformMatrix4fv(g_rt_u[9], 1, GL_TRUE, ivp);
    glUniform3fv(g_rt_u[10], 1, cam);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    for (int i = 6; i >= 0; i--) { glActiveTexture(GL_TEXTURE0 + i); glBindSampler(i, 0); }
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return g_rt.tex;
}
