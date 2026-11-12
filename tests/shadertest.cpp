// Translate every D3D9 shader blob in the game exe to GLSL and compile it with the system OpenGL.
#define GL_SILENCE_DEPRECATION
#include "../runtime/d3d9_shader.h"
#include "../runtime/pbr.h"
#include <SDL3/SDL.h>
#include <OpenGL/gl3.h>
#include <cstdio>
#include <vector>
#include <cstring>
#include <string>

static bool compile(GLenum type, const std::string &src, std::string &log) {
    GLuint sh = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    char buf[8192]; GLsizei n = 0; glGetShaderInfoLog(sh, sizeof buf, &n, buf);
    log.assign(buf, n);
    glDeleteShader(sh);
    return ok;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    std::vector<uint8_t> d;
    { fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(d.data(), 1, d.size(), f); fclose(f); }
    bool verbose = argc > 2;
    SDL_Init(SDL_INIT_VIDEO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_Window *w = SDL_CreateWindow("t", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx) { printf("no GL: %s\n", SDL_GetError()); return 1; }
    printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    int total = 0, tr_fail = 0, gl_fail = 0, pbr_total = 0;
    for (size_t off = 0; off + 8 <= d.size(); off += 4) {
        uint32_t v; memcpy(&v, &d[off], 4);
        uint32_t hi = v >> 16, maj = (v >> 8) & 0xFF;
        if ((hi != 0xFFFF && hi != 0xFFFE) || maj < 2 || maj > 3 || (v & 0xFF) > 1) continue;
        uint32_t c1; memcpy(&c1, &d[off + 4], 4);
        if ((c1 & 0xFFFF) != 0xFFFE) continue;   // fxc output starts with a CTAB comment
        const uint32_t *t = (const uint32_t *)&d[off];
        size_t maxt = (d.size() - off) / 4;
        size_t len = sm_length(t, maxt);
        if (!len) continue;
        total++;
        SmShader s;
        if (!sm_translate(t, len, s)) { tr_fail++; printf("%06zx %s_%d_%d: TRANSLATE FAIL: %s\n", off, s.ps ? "ps" : "vs", s.major, s.minor, s.error.c_str()); continue; }
        std::string src = s.ps ? sm_ps_source(s, nullptr) : sm_vs_source(s, nullptr);
        std::string log;
        if (!compile(s.ps ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, src, log)) {
            gl_fail++;
            printf("%06zx %s_%d_%d: GL FAIL\n%s\n", off, s.ps ? "ps" : "vs", s.major, s.minor, log.c_str());
            if (verbose) printf("%s\n", src.c_str());
        } else if (verbose) printf("%06zx ok\n", off);
        // shadertest <exe> v <name>: print the GLSL of every shader whose constant table names <name>
        if (argc > 3 && std::string((const char *)&d[off], len * 4).find(argv[3]) != std::string::npos)
        {
            uint64_t hh = 1469598103934665603ull;   // as d3d9.cpp make_shader
            for (size_t w = 0; w < len; w++) { hh ^= t[w]; hh *= 1099511628211ull; }
            if (s.ps) hh ^= 0x9e3779b97f4a7c15ull;
            printf("==== %06zx %s hash %016llx inputs:", off, s.ps ? "ps" : "vs", (unsigned long long)hh);
            for (auto &in : s.inputs) printf(" %s=%d/%d", in.reg.c_str(), in.usage, in.index);
            for (auto &d : s.fdefs) printf(" def c%d = %g %g %g %g;", d.first, d.second[0], d.second[1], d.second[2], d.second[3]);
            printf("\n%s\n", verbose ? src.c_str() : "");
        }
        // pbr=1 replacement of the car material (runtime/pbr.cpp)
        if (s.ps) {
            uint64_t hh = 1469598103934665603ull;
            for (size_t w = 0; w < len; w++) { hh ^= t[w]; hh *= 1099511628211ull; }
            hh ^= 0x9e3779b97f4a7c15ull;
            for (int v_rt = 0; v_rt < 2; v_rt++) {
            pbr_configure(true, v_rt);
            SmShader alt = s;
            std::string funcs = s.funcs, body;
            if (pbr_car_shader(hh, funcs, body)) {
                alt.funcs = funcs; alt.body = body;
                for (int v = 0; v < 2; v++) {
                    std::string psrc = sm_ps_source(alt, nullptr, 1 << 7, v);
                    pbr_total++;
                    if (!compile(GL_FRAGMENT_SHADER, psrc, log)) { gl_fail++; printf("%06zx pbr%s: GL FAIL\n%s\n", off, v ? " (velocity)" : "", log.c_str()); if (verbose) printf("%s\n", psrc.c_str()); }
                }
            }
            }
        }
        // temporal AA velocity variant
        std::string vsrc = s.ps ? sm_ps_source(s, nullptr, 0, true) : sm_vs_source(s, nullptr, true);
        if (!compile(s.ps ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, vsrc, log)) {
            gl_fail++;
            printf("%06zx %s_%d_%d (velocity): GL FAIL\n%s\n", off, s.ps ? "ps" : "vs", s.major, s.minor, log.c_str());
            if (verbose) printf("%s\n", vsrc.c_str());
        }
        off += (len - 1) * 4;
    }
    printf("total %d (+%d PBR variants), translate failures %d, GL failures %d\n", total, pbr_total, tr_fail, gl_fail);
    return 0;
}
