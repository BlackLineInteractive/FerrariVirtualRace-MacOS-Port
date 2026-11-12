// Direct3D 9 shader bytecode (SM 2.x / 3.0) -> GLSL 4.10 translator.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>

struct SmSemantic { int usage, index; uint8_t mask; std::string reg; };

struct SmShader {
    bool ps = false;
    int major = 0, minor = 0;
    std::string decls;     // global declarations (temps, uniforms, samplers, helper fns)
    std::string body;      // statements of main() (before output epilogue)
    std::string funcs;     // subroutines (labels)
    // VS: inputs by register (usage/index), outputs (usage/index/mask/reg) excluding position/psize
    // PS: inputs (usage/index/mask/reg)
    std::vector<SmSemantic> inputs, outputs;
    uint32_t sampler_type[16] = {};   // D3DSTT: 2 = 2D, 3 = cube, 4 = volume
    uint16_t samplers_used = 0;
    bool writes_depth = false, uses_vpos = false, uses_vface = false;
    bool writes_psize = false, writes_fog = false;
    int color_outputs = 1;
    std::string pos_reg, psize_reg;   // vs_3_0: output registers (with swizzle) for POSITION0 / PSIZE
    // constants defined inside the shader (def/defi/defb): the host patches them into the uniform arrays
    std::vector<std::pair<int, std::array<float, 4>>> fdefs;
    std::vector<std::pair<int, std::array<int, 4>>> idefs;
    std::vector<std::pair<int, int>> bdefs;
    std::string error;
};

// Parse and translate bytecode. Returns false on unsupported constructs (error filled in).
bool sm_translate(const uint32_t *tokens, size_t max_tokens, SmShader &out);
// Size of the bytecode in DWORDs (up to and including the END token), 0 on error.
size_t sm_length(const uint32_t *tokens, size_t max_tokens);

// Final GLSL sources for a VS/PS pair (varyings are matched by usage/index).
std::string sm_vs_source(const SmShader &vs, const SmShader *ps);
// shadow_mask: 2D samplers bound to depth textures (sampled as depth comparisons).
std::string sm_ps_source(const SmShader &ps, const SmShader *vs, uint16_t shadow_mask = 0);

// Name of the varying that carries a semantic.
std::string sm_varying(int usage, int index);
