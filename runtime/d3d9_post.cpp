// Anti-aliasing modes offered in the game's "Anti Aliasing" option.
//
// The game lists the D3D multisample types the device reports (index == type, see the table at
// 0x67a980 in FerrariVR_Hi.exe) and stores the chosen index in a global. We report types 2..6:
//   2 -> FXAA       post pass (FXAA 3.11 style, quality preset ~12)
//   3 -> MSAA 2x    real multisampling of the scene (d3d9.cpp, "MSAA shadows")
//   4 -> MSAA 4x
//   5 -> MSAA 8x
//   6 -> FSR 1      MSAA 4x, then AMD FidelityFX Super Resolution 1: EASU upscale to the window's
//                   native pixel size and RCAS sharpening
#define GL_SILENCE_DEPRECATION
#include "rt.h"
#include <OpenGL/gl3.h>
#include <string>
#include <cstring>
#include <cstdlib>

// Guest global holding the clamped "AntiAlias" index (FerrariVR_Hi.exe).
static const uint32_t kGuestAntiAlias = 0x9cf280;

struct AaMode { int post, samples; const char *label; };   // post: 0 none, 1 FXAA, 2 FSR 1
static const AaMode kModes[] = {
    {0, 0, nullptr}, {0, 0, nullptr}, {1, 0, "FXAA"}, {0, 2, "MSAA 2x"}, {0, 4, "MSAA 4x"}, {0, 8, "MSAA 8x"}, {2, 4, "FSR 1"},
};
static const int kNumModes = sizeof kModes / sizeof kModes[0];

static const AaMode &aa_mode() {
    static int forced = -2;
    if (forced == -2) {
        const char *e = getenv("FVR_AA");
        forced = -1;
        if (e) {
            forced = 0;
            static const char *names[] = {"off", "", "fxaa", "msaa2", "msaa4", "msaa8", "fsr"};
            for (int i = 0; i < kNumModes; i++) if (!strcmp(e, names[i])) forced = i;
        }
    }
    uint32_t v = forced >= 0 ? (uint32_t)forced : R32(kGuestAntiAlias);
    return kModes[v < (uint32_t)kNumModes ? v : 0];
}
int post_aa_mode() { return aa_mode().post; }
int post_msaa_samples() { return aa_mode().samples; }
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
