// Fixed-function pipeline emulation: GLSL generated from the D3D9 render/texture-stage state.
#include "d3d9_internal.h"
#include <cstdio>
#include <set>

enum { RS_LIGHTING = 137, RS_FOGENABLE = 28, RS_FOGTABLEMODE = 35, RS_FOGVERTEXMODE = 140, RS_SPECULARENABLE = 29,
       RS_COLORVERTEX = 141, RS_DIFFUSEMATSRC = 145, RS_SPECULARMATSRC = 146, RS_AMBIENTMATSRC = 147,
       RS_EMISSIVEMATSRC = 148, RS_NORMALIZENORMALS = 143, RS_LOCALVIEWER = 142, RS_RANGEFOG = 48 };
enum { TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
       TSS_TEXCOORDINDEX = 11, TSS_TTFF = 24, TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27, TSS_RESULTARG = 28 };

int ff_attrib_location(int usage, int index) {
    switch (usage) {
    case 0: case 9: return index == 0 ? 0 : -1;
    case 1: return 1;
    case 3: return 2;
    case 10: return index < 2 ? 3 + index : -1;
    case 5: return index < 8 ? 5 + index : -1;
    case 4: return 13;
    default: return -1;
    }
}

static const char *EPILOGUE =
    "    gl_Position = o_pos;\n"
    "    gl_Position.y = -gl_Position.y;\n"
    "    gl_Position.xy += posFixup.xy * gl_Position.w;\n"
    "    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n";

SmShader ff_ps_iface() {
    SmShader s;
    s.ps = true; s.major = 2;
    s.inputs.push_back({10, 0, 15, "d"});
    s.inputs.push_back({10, 1, 15, "s"});
    for (int i = 0; i < 8; i++) s.inputs.push_back({5, i, 15, "t"});
    s.inputs.push_back({11, 0, 1, "f"});
    return s;
}

SmShader ff_vs_iface(const Decl *, const DevState &) {
    SmShader s;
    s.major = 2;
    s.outputs.push_back({10, 0, 15, ""});
    s.outputs.push_back({10, 1, 15, ""});
    for (int i = 0; i < 8; i++) s.outputs.push_back({5, i, 15, ""});
    s.outputs.push_back({11, 0, 1, ""});
    return s;
}

std::string ff_vs_source(const DevState &st, const Decl *decl, const SmShader *ps, std::string &key) {
    bool has[16] = {};
    bool pret = decl && decl->pretransformed;
    if (decl) for (auto &e : decl->el) { int l = ff_attrib_location(e.usage, e.index); if (l >= 0) has[l] = true; }
    bool lighting = st.rs[RS_LIGHTING] && !pret;
    bool fog = st.rs[RS_FOGENABLE] != 0;
    int vfog = fog && st.rs[RS_FOGTABLEMODE] == 0 ? (int)st.rs[RS_FOGVERTEXMODE] : 0;
    char k[256];
    snprintf(k, sizeof k, "VS%d%d|%x|%d|%d%d%d%d%d|%d%d|%d", pret, lighting, [&] { int m = 0; for (int i = 0; i < 16; i++) m |= has[i] << i; return m; }(),
             vfog, st.rs[RS_COLORVERTEX] != 0, st.rs[RS_DIFFUSEMATSRC], st.rs[RS_SPECULARMATSRC], st.rs[RS_AMBIENTMATSRC],
             st.rs[RS_EMISSIVEMATSRC], st.rs[RS_SPECULARENABLE] != 0, st.rs[RS_LOCALVIEWER] != 0, st.rs[RS_RANGEFOG] != 0);
    key = k;
    std::string s = "#version 410 core\n";
    s += "uniform vec4 posFixup;\nuniform vec4 ffViewport;\n";
    s += "uniform mat4 ffWorld, ffView, ffProj;\n";
    s += "uniform vec4 ffMatDiffuse, ffMatAmbient, ffMatSpecular, ffMatEmissive;\nuniform float ffMatPower;\n";
    s += "uniform vec4 ffAmbient;\nuniform int ffNumLights;\n";
    s += "uniform vec4 ffLightPos[8], ffLightDir[8], ffLightDiff[8], ffLightSpec[8], ffLightAmb[8], ffLightAtt[8], ffLightSpot[8];\n";
    s += "uniform vec4 ffFog;\n";   // start, end, density
    const char *names[] = {"a_pos", "a_bw", "a_nrm", "a_col0", "a_col1", "a_tc0", "a_tc1", "a_tc2", "a_tc3", "a_tc4", "a_tc5", "a_tc6", "a_tc7", "a_psize"};
    for (int i = 0; i < 14; i++) if (has[i]) s += "layout(location = " + std::to_string(i) + ") in vec4 " + names[i] + ";\n";
    std::set<std::string> outs = {"V_10_0", "V_10_1", "V_11_0"};
    for (int i = 0; i < 8; i++) outs.insert("V_5_" + std::to_string(i));
    std::vector<std::string> extra;
    if (ps) for (auto &i : ps->inputs) { std::string v = sm_varying(i.usage, i.index); if (!outs.count(v)) { outs.insert(v); extra.push_back(v); } }
    for (auto &o : outs) s += "out vec4 " + o + ";\n";
    s += "void main() {\n";
    s += "    vec4 o_pos;\n    vec4 ep = vec4(0.0, 0.0, 0.0, 1.0);\n";
    s += has[0] ? "    vec4 p = a_pos;\n" : "    vec4 p = vec4(0.0, 0.0, 0.0, 1.0);\n";
    if (pret) {
        s += "    float w = p.w == 0.0 ? 1.0 : 1.0 / p.w;\n";
        s += "    vec2 ndc = vec2((p.x - ffViewport.x) / ffViewport.z * 2.0 - 1.0, 1.0 - (p.y - ffViewport.y) / ffViewport.w * 2.0);\n";
        s += "    o_pos = vec4(ndc * w, p.z * w, w);\n";
    } else {
        s += "    ep = ffView * (ffWorld * vec4(p.xyz, 1.0));\n";
        s += "    o_pos = ffProj * ep;\n";
    }
    s += has[3] ? "    vec4 vd = a_col0;\n" : "    vec4 vd = vec4(1.0);\n";
    s += has[4] ? "    vec4 vs = a_col1;\n" : "    vec4 vs = vec4(0.0);\n";
    if (lighting) {
        auto src = [&](int rs, const char *mat) -> std::string {
            if (!st.rs[RS_COLORVERTEX]) return mat;
            if (st.rs[rs] == 1 && has[3]) return "vd";
            if (st.rs[rs] == 2 && has[4]) return "vs";
            return mat;
        };
        s += "    vec4 mD = " + src(RS_DIFFUSEMATSRC, "ffMatDiffuse") + ";\n";
        s += "    vec4 mA = " + src(RS_AMBIENTMATSRC, "ffMatAmbient") + ";\n";
        s += "    vec4 mS = " + src(RS_SPECULARMATSRC, "ffMatSpecular") + ";\n";
        s += "    vec4 mE = " + src(RS_EMISSIVEMATSRC, "ffMatEmissive") + ";\n";
        s += has[2] ? "    vec3 N = normalize(mat3(ffView * ffWorld) * a_nrm.xyz);\n" : "    vec3 N = vec3(0.0);\n";
        s += "    vec3 V = " + std::string(st.rs[RS_LOCALVIEWER] ? "normalize(-ep.xyz)" : "vec3(0.0, 0.0, -1.0)") + ";\n";
        s += "    vec3 amb = ffAmbient.rgb, dif = vec3(0.0), spe = vec3(0.0);\n";
        s += "    for (int i = 0; i < ffNumLights; i++) {\n"
             "        vec3 L; float att = 1.0;\n"
             "        if (ffLightSpot[i].w == 3.0) { L = -ffLightDir[i].xyz; }\n"
             "        else {\n"
             "            vec3 d = ffLightPos[i].xyz - ep.xyz; float dist = length(d); L = d / max(dist, 1e-6);\n"
             "            if (dist > ffLightAtt[i].w) att = 0.0;\n"
             "            else att = 1.0 / max(ffLightAtt[i].x + ffLightAtt[i].y * dist + ffLightAtt[i].z * dist * dist, 1e-6);\n"
             "            if (ffLightSpot[i].w == 2.0) {\n"
             "                float rho = dot(-L, normalize(ffLightDir[i].xyz));\n"
             "                if (rho <= ffLightSpot[i].y) att = 0.0;\n"
             "                else if (rho <= ffLightSpot[i].x) att *= pow(clamp((rho - ffLightSpot[i].y) / max(ffLightSpot[i].x - ffLightSpot[i].y, 1e-6), 0.0, 1.0), ffLightSpot[i].z);\n"
             "            }\n"
             "        }\n"
             "        amb += ffLightAmb[i].rgb * att;\n"
             "        float nl = max(dot(N, L), 0.0);\n"
             "        dif += ffLightDiff[i].rgb * nl * att;\n"
             "        if (nl > 0.0) spe += ffLightSpec[i].rgb * pow(max(dot(N, normalize(L + V)), 0.0), ffMatPower) * att;\n"
             "    }\n";
        s += "    V_10_0 = vec4(clamp(mE.rgb + mA.rgb * amb + mD.rgb * dif, 0.0, 1.0), mD.a);\n";
        s += st.rs[RS_SPECULARENABLE] ? "    V_10_1 = vec4(clamp(mS.rgb * spe, 0.0, 1.0), vs.a);\n" : "    V_10_1 = vec4(0.0, 0.0, 0.0, vs.a);\n";
    } else {
        s += "    V_10_0 = vd;\n    V_10_1 = vs;\n";
    }
    for (int i = 0; i < 8; i++)
        s += has[5 + i] ? "    V_5_" + std::to_string(i) + " = a_tc" + std::to_string(i) + ";\n"
                        : "    V_5_" + std::to_string(i) + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
    // vertex fog factor
    if (pret) s += "    V_11_0 = vec4(vs.a, 0.0, 0.0, 1.0);\n";
    else if (vfog) {
        std::string dist = st.rs[RS_RANGEFOG] ? "length(ep.xyz)" : "abs(ep.z)";
        s += "    float fd = " + dist + ";\n";
        if (vfog == 3) s += "    float ff = (ffFog.y - fd) / max(ffFog.y - ffFog.x, 1e-6);\n";
        else if (vfog == 1) s += "    float ff = exp(-fd * ffFog.z);\n";
        else s += "    float ff = exp(-(fd * ffFog.z) * (fd * ffFog.z));\n";
        s += "    V_11_0 = vec4(clamp(ff, 0.0, 1.0), 0.0, 0.0, 1.0);\n";
    } else s += "    V_11_0 = vec4(1.0, 0.0, 0.0, 1.0);\n";
    for (auto &v : extra) s += "    " + v + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
    s += EPILOGUE;
    if (has[13]) s += "    gl_PointSize = a_psize.x;\n";
    s += "}\n";
    return s;
}

// ------------------------------------------------------------------ pixel stage
static std::string arg(int a, int stage, const char *tex) {
    std::string v;
    switch (a & 7) {
    case 0: v = "dif"; break;
    case 1: v = "cur"; break;
    case 2: v = tex; break;
    case 3: v = "ffTFactor"; break;
    case 4: v = "spc"; break;
    case 5: v = "tmp"; break;
    case 6: v = "ffConst[" + std::to_string(stage) + "]"; break;
    default: v = "cur"; break;
    }
    if (a & 0x20) v = "vec4(" + v + ".a)";
    if (a & 0x10) v = "(vec4(1.0) - " + v + ")";
    return v;
}

static std::string op_expr(int op, const std::string &a0, const std::string &a1, const std::string &a2, const char *tex) {
    switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 + " * " + a2;
    case 5: return a1 + " * " + a2 + " * 2.0";
    case 6: return a1 + " * " + a2 + " * 4.0";
    case 7: return a1 + " + " + a2;
    case 8: return a1 + " + " + a2 + " - 0.5";
    case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2.0";
    case 10: return a1 + " - " + a2;
    case 11: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
    case 12: return "mix(" + a2 + ", " + a1 + ", dif.a)";
    case 13: return std::string("mix(") + a2 + ", " + a1 + ", " + tex + ".a)";
    case 14: return "mix(" + a2 + ", " + a1 + ", ffTFactor.a)";
    case 15: return a1 + " + " + a2 + " * (1.0 - " + tex + ".a)";
    case 16: return "mix(" + a2 + ", " + a1 + ", cur.a)";
    case 17: return a1;
    case 18: return "vec4(" + a1 + ".rgb + " + a1 + ".a * " + a2 + ".rgb, " + a1 + ".a)";
    case 19: return "vec4(" + a1 + ".rgb * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case 20: return "vec4((1.0 - " + a1 + ".a) * " + a2 + ".rgb + " + a1 + ".rgb, " + a1 + ".a)";
    case 21: return "vec4((1.0 - " + a1 + ".rgb) * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case 24: return "vec4(clamp(4.0 * dot(" + a1 + ".rgb - 0.5, " + a2 + ".rgb - 0.5), 0.0, 1.0))";
    case 25: return a0 + " + " + a1 + " * " + a2;
    case 26: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
    default: return a1;
    }
}

std::string ff_ps_source(const DevState &st, const SmShader *vs, std::string &key) {
    (void)vs;
    std::string k = "PS";
    int nstages = 0;
    int ttype[8] = {};
    for (int i = 0; i < 8; i++) {
        if (st.tss[i][TSS_COLOROP] == 1) break;
        nstages = i + 1;
        Texture *t = static_cast<Texture *>(st.tex[i]);
        ttype[i] = !t ? 0 : t->kind == K_CUBE ? 3 : t->kind == K_VOL ? 4 : 2;
        char b[128];
        snprintf(b, sizeof b, "|%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u", st.tss[i][1], st.tss[i][2], st.tss[i][3], st.tss[i][4],
                 st.tss[i][5], st.tss[i][6], st.tss[i][26], st.tss[i][27], st.tss[i][28], st.tss[i][11] & 7, ttype[i], st.tss[i][TSS_TTFF] & 0x100);
        k += b;
    }
    bool spec = st.rs[RS_SPECULARENABLE] != 0;
    k += spec ? "S" : "s";
    key = k;

    std::string s = "#version 410 core\n";
    s += "uniform vec4 ffTFactor;\nuniform vec4 ffConst[8];\n";
    s += "uniform int alphaFunc;\nuniform float alphaRef;\nuniform vec4 fogColor;\nuniform vec4 fogParams;\n";
    for (int i = 0; i < nstages; i++)
        if (ttype[i]) s += std::string("uniform ") + (ttype[i] == 3 ? "samplerCube" : ttype[i] == 4 ? "sampler3D" : "sampler2D") + " ps_s" + std::to_string(i) + ";\n";
    s += "in vec4 V_10_0;\nin vec4 V_10_1;\nin vec4 V_11_0;\n";
    for (int i = 0; i < 8; i++) s += "in vec4 V_5_" + std::to_string(i) + ";\n";
    s += "layout(location = 0) out vec4 oC0;\n";
    s += "bool atest(float a) {\n"
         "    float r = alphaRef;\n"
         "    if (alphaFunc == 1) return false;\n    if (alphaFunc == 2) return a < r;\n"
         "    if (alphaFunc == 3) return abs(a - r) < 0.5 / 255.0;\n    if (alphaFunc == 4) return a <= r;\n"
         "    if (alphaFunc == 5) return a > r;\n    if (alphaFunc == 6) return abs(a - r) >= 0.5 / 255.0;\n"
         "    if (alphaFunc == 7) return a >= r;\n    return true;\n}\n";
    s += "void main() {\n";
    s += "    vec4 dif = V_10_0, spc = V_10_1, cur = V_10_0, tmp = vec4(0.0);\n";
    for (int i = 0; i < nstages; i++) {
        const uint32_t *t = st.tss[i];
        std::string tex = "tex" + std::to_string(i);
        std::string tc = "V_5_" + std::to_string(t[TSS_TEXCOORDINDEX] & 7);
        bool proj = t[TSS_TTFF] & 0x100;
        if (!ttype[i]) s += "    vec4 " + tex + " = vec4(1.0);\n";
        else if (ttype[i] == 2) s += "    vec4 " + tex + " = " + (proj ? "textureProj(ps_s" + std::to_string(i) + ", " + tc + ".xyw)" : "texture(ps_s" + std::to_string(i) + ", " + tc + ".xy)") + ";\n";
        else s += "    vec4 " + tex + " = texture(ps_s" + std::to_string(i) + ", " + tc + ".xyz);\n";
        std::string c = op_expr((int)t[TSS_COLOROP], arg(t[TSS_COLORARG0], i, tex.c_str()), arg(t[TSS_COLORARG1], i, tex.c_str()), arg(t[TSS_COLORARG2], i, tex.c_str()), tex.c_str());
        std::string a = t[TSS_ALPHAOP] == 1 ? "cur" :
            op_expr((int)t[TSS_ALPHAOP], arg(t[TSS_ALPHAARG0], i, tex.c_str()), arg(t[TSS_ALPHAARG1], i, tex.c_str()), arg(t[TSS_ALPHAARG2], i, tex.c_str()), tex.c_str());
        std::string dst = t[TSS_RESULTARG] == 5 ? "tmp" : "cur";
        s += "    { vec4 c_ = clamp(" + c + ", 0.0, 1.0); vec4 a_ = clamp(" + a + ", 0.0, 1.0); " + dst + " = vec4(c_.rgb, a_.a); }\n";
    }
    s += "    oC0 = cur;\n";
    if (spec) s += "    oC0.rgb = clamp(oC0.rgb + spc.rgb, 0.0, 1.0);\n";
    s += "    if (fogParams.x > 0.0) {\n"
         "        float f = V_11_0.x;\n"
         "        if (fogParams.x >= 2.0) {\n"
         "            float z = 1.0 / gl_FragCoord.w;\n"
         "            if (fogParams.x == 2.0) f = (fogParams.z - z) / max(fogParams.z - fogParams.y, 1e-6);\n"
         "            else if (fogParams.x == 3.0) f = exp(-z * fogParams.w);\n"
         "            else f = exp(-(z * fogParams.w) * (z * fogParams.w));\n"
         "        }\n"
         "        oC0.rgb = mix(fogColor.rgb, oC0.rgb, clamp(f, 0.0, 1.0));\n"
         "    }\n";
    s += "    // alphaFunc >= 16: alpha test as alpha-to-coverage (multisampled target), sharpened by the alpha gradient\n";
    s += "    if (alphaFunc >= 16) {\n";
    s += "        float c = clamp((oC0.a - alphaRef) / max(fwidth(oC0.a), 1e-4) + 0.5, 0.0, 1.0);\n";
    s += "        int N = max(gl_NumSamples, 1), n = int(c * float(N) + 0.5);\n";
    s += "        if (n == 0) discard;\n";
    s += "        int full = N >= 32 ? -1 : (1 << N) - 1, m = n >= N ? full : (1 << n) - 1;\n";
    s += "        int r = (int(gl_FragCoord.x) + 3 * int(gl_FragCoord.y)) % N;\n";
    s += "        gl_SampleMask[0] = ((m << r) | (m >> (N - r))) & full;\n";
    s += "    } else if (!atest(oC0.a)) discard;\n";
    s += "}\n";
    return s;
}
