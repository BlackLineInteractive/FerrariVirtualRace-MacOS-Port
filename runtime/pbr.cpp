// Physically based replacement for the game's car material (pbr=1 in options.txt / FVR_PBR=1).
//
// The game shades every car part with one pixel shader family whose constants come from the car's
// .MTL files: DiffColor c0, AmbColor c1, SpecColor c2, MetalColor c3, ReflColor c4, SpecSize c7
// (Phong exponent, scale, exponent of a broad "metal flake" lobe), FallOffLevel c8, FresnelLevel c9
// (F(N.V) = exp2(-N.V * z) * x + y), hemisphere ambient AmbObjL/H c10/c11, sun LightVec c16 /
// SunColor c17, cascaded shadows (SelCascade c35, ShadowMats c36-c47, ShadowMap s7) and the
// reflection cube EnvMapTex s9. Glass is the same shader, untextured, with DiffColor.w < 1 and
// premultiplied blending.
//
// The replacement keeps the game's inputs, diffuse lighting and shadows and swaps the specular
// side for a physically based one:
//   - GGX / Smith specular with Schlick Fresnel; roughness from the Phong exponent
//     (alpha = sqrt(2 / (n + 2))); F0 from the game's own Fresnel at normal incidence;
//   - parts whose F0 is high (chrome) become metallic: no diffuse, coloured reflection;
//   - glossy dielectrics (paint, glass, plastics) get an energy-conserving coat: what the coat
//     reflects is taken from the layer below;
//   - reflections use the split-sum environment BRDF (Karis' analytic fit), blurred by roughness;
//   - glass reflects more and lets less through at grazing angles (alpha = a + (1 - a) F).
// The reflection source is the game's cube map for now; ray-traced reflections replace it later.
#include "pbr.h"
#include <cstdlib>
#include <cstring>
#include <string>

static bool g_rt;   // pbr_configure

namespace {

// helpers placed before main()
const char *kFuncs = R"GLSL(
// Karis' analytic fit of the split-sum environment BRDF: scale and bias for F0
vec2 pbr_env_brdf(float NV, float rough) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
vec3 pbr_schlick(vec3 F0, float c) { float f = pow(1.0 - c, 5.0); return F0 + (1.0 - F0) * f; }
float pbr_ggx(float NH, float a) { float a2 = a * a, d = NH * NH * (a2 - 1.0) + 1.0; return a2 / (3.14159265 * d * d); }
float pbr_vis(float NV, float NL, float a) {   // height-correlated Smith visibility
    float a2 = a * a;
    float gv = NL * sqrt(NV * NV * (1.0 - a2) + a2), gl = NV * sqrt(NL * NL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}
)GLSL";

// N, V: normal and direction to the eye; S: shadow position; FOG: fog factor; V0: vertex ambient;
// UV: texture coordinates (textured variant only)
std::string body(const char *N, const char *V, const char *S, const char *FOG, const char *V0, const char *UV, bool textured) {
    std::string b = R"GLSL(
    // ---- FVR physically based car material (runtime/pbr.cpp)
    vec3 p_N = normalize(@N.xyz);
    vec3 p_V = normalize(@V.xyz);
    float p_NV = clamp(dot(p_N, p_V), 1e-4, 1.0);
    vec3 p_L = normalize(pc[16].xyz);
    float p_NL = clamp(dot(p_N, p_L), 0.0, 1.0);
    vec3 p_H = normalize(p_L + p_V);
    float p_NH = clamp(dot(p_N, p_H), 0.0, 1.0), p_VH = clamp(dot(p_V, p_H), 0.0, 1.0);
    vec3 p_R = reflect(-p_V, p_N);
    // sun shadow: the game's cascade selection
    float p_sh = pc[6].x;
    if (pb[1]) {
        float p_c = log1(@S.z * pc[35].x + pc[35].y) * pc[35].z + pc[35].w;
        p_c = min(max(p_c, pc[6].y), pc[6].z);
        p_c -= fract(p_c);
        vec4 p_k = vec4(p_c) + pc[5];
        vec4 p_m0 = vec4(pc[6].y), p_m1 = vec4(pc[6].y), p_m2 = vec4(pc[6].y);
        for (int i = 0; i < 4; i++) if (p_k[i] == 0.0) { p_m0 = pc[36 + 3 * i]; p_m1 = pc[37 + 3 * i]; p_m2 = pc[38 + 3 * i]; }
        vec4 p_s = @S.xyzx * pc[6].xxxy + pc[6].wwwx;
        p_sh = t2p_7(vec4(dot(p_s, p_m0), dot(p_s, p_m1), dot(p_s, p_m2), pc[6].x)).x;
    }
    vec3 p_sun = pc[17].rgb * p_sh;
    // material
    vec4 p_tex = vec4(1.0);
)GLSL";
    if (textured) b += R"GLSL(
    p_tex = t2_0(@UV);
    p_tex.rgb = mix(p_tex.rgb, vec3(dot(p_tex.rgb, vec3(0.3086, 0.6094, 0.082))), pc[20].w);
)GLSL";
    b += R"GLSL(
    float p_F0g = clamp(pc[9].y + pc[9].x * exp2(-pc[9].z), 0.0, 1.0);
    float p_F90g = clamp(pc[9].x + pc[9].y, 0.0, 1.0);
    float p_refl = max(pc[4].r, max(pc[4].g, pc[4].b));
    bool p_glossy = p_F90g > 0.1 && p_refl > 0.0;
    float p_a = p_glossy ? sqrt(2.0 / (max(pc[7].x, 1.0) + 2.0)) : 0.6;
    p_a = clamp(p_a, 0.03, 1.0);
    float p_rough = sqrt(p_a);
    float p_metal = smoothstep(0.15, 0.45, p_F0g);
    vec3 p_F0 = mix(vec3(0.04), pc[4].rgb * p_F0g, p_metal);
    // the game's diffuse lighting: sun with wrap and rim fall-off, hemisphere ambient, vertex ambient,
    // the broad metal-flake lobe tinted by MetalColor (with 0.7 of the ambient), AmbColor, all x2
    float p_wrap = p_NL * (1.0 - pow(1.0 - p_NL, 3.0));
    float p_fall = exp2(-p_NV * pc[8].z) * pc[8].x + pc[8].y;
    float p_up = clamp(p_N.y, 0.0, 1.0);
    vec3 p_amb = mix(pc[10].rgb, pc[11].rgb, p_up * p_up) + @V0.rgb;
    float p_RL = clamp(dot(p_R, p_L), 0.0, 1.0);
    vec3 p_flake = (p_sun * exp2(log1(p_RL) * pc[7].z) + 0.7 * p_amb) * pc[3].rgb;
    vec3 p_lit = 2.0 * p_tex.rgb * (pc[0].rgb * (p_sun * p_wrap * p_fall + p_amb) + p_flake + pc[1].rgb);
    // specular: sun (GGX) and environment (split sum)
    vec3 p_Fv = pbr_schlick(p_F0, p_NV);
    vec3 p_Fh = pbr_schlick(p_F0, p_VH);
    vec3 p_spec = p_sun * p_NL * pbr_ggx(p_NH, p_a) * pbr_vis(p_NV, p_NL, p_a) * p_Fh * clamp(pc[2].rgb * 1.25, 0.0, 1.0);
    vec2 p_ab = pbr_env_brdf(p_NV, p_rough);
    vec3 p_env = textureLod(ps_s9, p_R, p_rough * 7.0).rgb;
    float p_envw = p_metal > 0.5 ? 1.0 : p_refl;
    vec3 p_envs = p_env * (p_F0 * p_ab.x + p_ab.y) * p_envw;
    // energy: what the coat reflects does not reach the layer below; metals have no diffuse
    float p_coat = dot(p_Fv, vec3(0.3333));
    float p_a0 = pc[0].w;
)GLSL";
    if (g_rt) b += R"GLSL(
    // ray-traced reflections: mirror-like pixels (GGX alpha <= 0.15) get one ray instead of the
    // cube map; the weight is what the cube map would have been multiplied by
    float p_w = dot((p_F0 * p_ab.x + p_ab.y) * p_envw, vec3(0.3333));
    bool p_ray = p_a <= 0.15 && p_w > 0.003;
    if (p_ray) p_envs = vec3(0.0);
    oRtM0 = vec4(p_R, p_a);
    oRtM1 = vec4(-@V.xyz, p_ray ? p_w : 0.0);
)GLSL";
    if (textured) b += R"GLSL(
    vec3 p_col = p_lit * (1.0 - p_metal) * (1.0 - p_coat) + p_spec + p_envs;
    float p_alpha = p_tex.a * pc[0].w;
)GLSL";
    else b += R"GLSL(
    // premultiplied, as the game: opaque parts have DiffColor.w = 1, glass less
    vec3 p_col = p_lit * p_a0 * (1.0 - p_metal) * (1.0 - p_coat) + p_spec + p_envs;
    float p_alpha = p_a0 + (1.0 - p_a0) * p_coat;
)GLSL";
    b += R"GLSL(
    oC0.rgb = mix(pc[29].rgb, p_col, @FOG.x);
    oC0.a = p_alpha;
)GLSL";
    auto sub = [&](const char *k, const char *v) {
        for (size_t p; (p = b.find(k)) != std::string::npos;) b.replace(p, strlen(k), v);
    };
    sub("@N", N); sub("@V0", V0); sub("@V", V); sub("@S", S); sub("@FOG", FOG); sub("@UV", UV);
    return b;
}

}   // namespace

static int g_pbr = 0;
void pbr_configure(bool on, bool rt) { g_pbr = on; g_rt = on && rt; }

bool pbr_car_shader(uint64_t hash, std::string &funcs, std::string &main_body) {
    if (g_pbr <= 0) return false;
    switch (hash) {
    case 0x9b3e710c007aafdeull:   // untextured: paint, glass, chrome, rubber
        main_body = body("v1", "v2", "v3", "v4", "v0", "", false);
        break;
    case 0xb4a380d49485c1c8ull:   // diffuse texture (alpha = texture alpha)
        main_body = body("v1", "v2", "v4", "v5", "v0", "v3", true);
        break;
    default:
        return false;
    }
    funcs += kFuncs;
    if (g_rt) funcs += "layout(location = 2) out vec4 oRtM0;\nlayout(location = 3) out vec4 oRtM1;\n";
    return true;
}

bool pbr_writes_mask() { return g_rt; }
