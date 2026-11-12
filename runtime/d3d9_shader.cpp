// Direct3D 9 shader bytecode -> GLSL 4.10 translator (vs_2_0/2_x/3_0, ps_2_0/2_x/3_0).
//
// Conventions shared with the D3D9 device:
//   VS float/int/bool constants: uniform vec4 vc[256], ivec4 vi[16], bool vb[16]   (ps: pc/pi/pb)
//   VS input register N        : layout(location = N) in vec4 vN
//   PS samplers sN             : uniform samplerXX ps_sN  (texture unit N)
//   VS samplers sN             : uniform samplerXX vs_sN  (texture unit 16 + N)
//   varyings                   : V_<usage>_<index>, components packed from .x
//   rendering is Y-flipped into FBOs, depth mapped from [0,1] to [-1,1].
#include "d3d9_shader.h"
#include <map>
#include <set>
#include <cstdio>
#include <cmath>
#include <cstring>

namespace {

enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_OUTPUT = 6,
    RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11, RT_CONST3 = 12,
    RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPF16 = 16, RT_MISC = 17, RT_LABEL = 18, RT_PRED = 19,
};
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT,
    OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2,
    OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM,
    OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81,
    OP_CMP = 88, OP_BEM = 89, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94,
    OP_TEXLDL = 95, OP_BREAKP = 96, OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};

struct Param {
    uint32_t tok = 0, rel = 0;
    bool has_rel = false;
    int type = 0, num = 0;
};

static int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int reg_num(uint32_t t) { return (int)(t & 0x7FF); }
static const char *CMPOP[8] = {"?", ">", "==", ">=", "<", "!=", "<=", "?"};

static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char *f, ...) {
    char b[1024];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

static std::string mask_str(uint32_t m) {
    std::string s;
    for (int i = 0; i < 4; i++) if (m & (1 << i)) s += "xyzw"[i];
    return s;
}
static int popcnt4(uint32_t m) { return __builtin_popcount(m & 15); }

struct Tr {
    const uint32_t *t;
    size_t n, pos = 0;
    SmShader &S;
    bool ps = false;
    int major = 0, minor = 0;
    std::string *out;
    std::string main_body, funcs;
    int indent = 1;
    int max_temp = -1;
    bool uses_a0 = false, uses_aL = false, uses_p0 = false;
    std::set<int> in_regs, t_regs, out_regs, color_outs, od_regs, ot_regs;
    bool uses_ofog = false, uses_opts = false, uses_opos = false, uses_odepth = false;
    std::map<int, SmSemantic> vs_in;               // VS input register -> semantic
    std::vector<SmSemantic> out_dcl, in_dcl;       // SM3 output / PS input declarations
    int loop_depth = 0, rep_count = 0;
    std::vector<int> loop_kind;                    // 0 rep, 1 loop (for aL save)
    std::set<int> labels_called;
    bool in_label = false;
    bool ok = true;

    Tr(const uint32_t *tok, size_t nt, SmShader &s) : t(tok), n(nt), S(s) { out = &main_body; }

    void fail(const std::string &e) { if (ok) S.error = e; ok = false; }
    void line(const std::string &s) { out->append(indent * 4, ' '); out->append(s); out->push_back('\n'); }

    Param read_param(bool src) {
        Param p;
        if (pos >= n) { fail("truncated"); return p; }
        p.tok = t[pos++];
        p.type = reg_type(p.tok);
        p.num = reg_num(p.tok);
        if (p.tok & (1u << 13)) {
            p.has_rel = true;
            if (major >= 2) { if (pos < n) p.rel = t[pos++]; }
            else p.rel = 0xB0E40000u | (RT_ADDR << 28);   // a0.x
        }
        (void)src;
        return p;
    }

    // ---------------------------------------------------------------- register names
    std::string cprefix() const { return ps ? "pc" : "vc"; }

    std::string rel_index(const Param &p) {
        int rt = reg_type(p.rel);
        int comp = (p.rel >> 16) & 3;
        if (rt == RT_LOOP) { uses_aL = true; return "aL"; }
        uses_a0 = true;
        return fmt("a0.%c", "xyzw"[comp]);
    }

    std::string reg_name(const Param &p, bool is_dst) {
        int num = p.num;
        switch (p.type) {
        case RT_TEMP: case RT_TEMPF16:
            if (num > max_temp) max_temp = num;
            return fmt("r%d", num);
        case RT_INPUT:
            if (p.has_rel) {   // v[aL + n] (SM3) - index into a local copy array is not supported
                fail("relative input addressing"); return "v0";
            }
            in_regs.insert(num);
            return fmt("v%d", num);
        case RT_CONST:
            if (p.has_rel) return fmt("%s[%s + %d]", cprefix().c_str(), rel_index(p).c_str(), num);
            return fmt("%s[%d]", cprefix().c_str(), num);
        case RT_CONST2: return fmt("%s[%d]", cprefix().c_str(), num + 2048);
        case RT_CONST3: return fmt("%s[%d]", cprefix().c_str(), num + 4096);
        case RT_CONST4: return fmt("%s[%d]", cprefix().c_str(), num + 6144);
        case RT_ADDR:
            if (ps) { t_regs.insert(num); return fmt("t%d", num); }
            uses_a0 = true;
            return "a0";
        case RT_RASTOUT:
            if (num == 0) { uses_opos = true; return "o_pos"; }
            if (num == 1) { uses_ofog = true; return "o_fog"; }
            uses_opts = true; return "o_pts";
        case RT_ATTROUT: od_regs.insert(num); return fmt("oD%d", num);
        case RT_OUTPUT:
            if (major >= 3) { out_regs.insert(num); return fmt("o%d", num); }
            ot_regs.insert(num); return fmt("oT%d", num);
        case RT_CONSTINT: return fmt("%s[%d]", ps ? "pi" : "vi", num);
        case RT_COLOROUT: color_outs.insert(num); return fmt("oC%d", num);
        case RT_DEPTHOUT: uses_odepth = true; return "o_depth";
        case RT_SAMPLER: return fmt("%s_s%d", ps ? "ps" : "vs", num);
        case RT_CONSTBOOL: return fmt("%s[%d]", ps ? "pb" : "vb", num);
        case RT_LOOP: uses_aL = true; return "aL";
        case RT_MISC:
            if (num == 0) { S.uses_vpos = true; return "vpos"; }
            S.uses_vface = true; return "vface";
        case RT_PRED: uses_p0 = true; return "p0";
        default:
            fail(fmt("register type %d", p.type));
            return "r0";
        }
        (void)is_dst;
    }

    // source operand as a vec4 expression
    std::string src(const Param &p) {
        std::string base = reg_name(p, false);
        if (p.type == RT_ADDR && !ps) base = "vec4(a0)";
        else if (p.type == RT_LOOP) base = "vec4(float(aL))";
        else if (p.type == RT_CONSTINT) base = "vec4(" + base + ")";
        else if (p.type == RT_CONSTBOOL) base = "vec4(" + base + " ? 1.0 : 0.0)";
        else if (p.type == RT_MISC && p.num == 1) base = "vec4(vface)";
        uint32_t sw = (p.tok >> 16) & 0xFF;
        std::string e = base;
        if (sw != 0xE4) {
            e += '.';
            for (int i = 0; i < 4; i++) e += "xyzw"[(sw >> (2 * i)) & 3];
        }
        switch ((p.tok >> 24) & 0xF) {
        case 0: return e;
        case 1: return "(-" + e + ")";
        case 2: return "(" + e + " - 0.5)";
        case 3: return "(0.5 - " + e + ")";
        case 4: return "(2.0 * " + e + " - 1.0)";
        case 5: return "(1.0 - 2.0 * " + e + ")";
        case 6: return "(1.0 - " + e + ")";
        case 7: return "(2.0 * " + e + ")";
        case 8: return "(-2.0 * " + e + ")";
        case 9: { std::string b = base; return "(" + e + " / " + b + ".z)"; }
        case 10: { std::string b = base; return "(" + e + " / " + b + ".w)"; }
        case 11: return "abs(" + e + ")";
        case 12: return "(-abs(" + e + "))";
        default: fail("src modifier NOT"); return e;
        }
    }
    // scalar from a source (first component of the swizzled value)
    std::string srcx(const Param &p) { return "(" + src(p) + ").x"; }
    // bool predicate source
    std::string pred(const Param &p, int comp_override = -1) {
        uses_p0 = true;
        uint32_t sw = (p.tok >> 16) & 0xFF;
        int c = comp_override >= 0 ? comp_override : (int)(sw & 3);
        std::string e = fmt("p0.%c", "xyzw"[c]);
        if (((p.tok >> 24) & 0xF) == 13) e = "!" + e;
        return e;
    }
    std::string pred4(const Param &p) {
        uses_p0 = true;
        uint32_t sw = (p.tok >> 16) & 0xFF;
        std::string e = "p0.";
        for (int i = 0; i < 4; i++) e += "xyzw"[(sw >> (2 * i)) & 3];
        if (((p.tok >> 24) & 0xF) == 13) e = "not(" + e + ")";
        return e;
    }

    // assign a vec4 expression to a destination honoring mask/saturate/predicate
    void assign(const Param &d, const std::string &expr, const Param *predp = nullptr) {
        uint32_t m = (d.tok >> 16) & 0xF;
        uint32_t mod = (d.tok >> 20) & 0xF;
        std::string name = reg_name(d, true);
        std::string e = expr;
        if (mod & 1) e = "clamp(" + e + ", 0.0, 1.0)";
        if (d.type == RT_ADDR && !ps) {       // mova
            e = "ivec4(round(" + e + "))";
            if (predp) e = "ivec4(mix(vec4(a0), vec4(" + e + "), bvec4(" + pred4(*predp) + ")))";
        } else if (d.type == RT_PRED) {
            fail("write to predicate via arithmetic");
            return;
        } else if (predp) {
            e = "mix(" + name + ", " + e + ", bvec4(" + pred4(*predp) + "))";
        }
        if (m == 0xF) line(name + " = " + e + ";");
        else {
            std::string ms = mask_str(m);
            line(name + "." + ms + " = (" + e + ")." + ms + ";");
        }
    }

    std::string row(const Param &p, int k) {   // register p offset by k (matrix rows)
        Param q = p;
        q.num += k;
        q.tok = (q.tok & ~0x7FFu) | (uint32_t)(q.num & 0x7FF);
        return src(q);
    }

    // ---------------------------------------------------------------- declarations
    void do_dcl() {
        uint32_t dt = t[pos++];
        Param d = read_param(false);
        uint32_t m = (d.tok >> 16) & 0xF;
        if (d.type == RT_SAMPLER) {
            uint32_t ty = (dt >> 27) & 0xF;
            if (d.num < 16) { S.sampler_type[d.num] = ty ? ty : 2; S.samplers_used |= 1 << d.num; }
            return;
        }
        int usage = (int)(dt & 0x1F), uidx = (int)((dt >> 16) & 0xF);
        if (!ps) {
            if (d.type == RT_INPUT) { vs_in[d.num] = {usage, uidx, (uint8_t)m, fmt("v%d", d.num)}; in_regs.insert(d.num); }
            else if (d.type == RT_OUTPUT) { out_dcl.push_back({usage, uidx, (uint8_t)m, fmt("o%d", d.num)}); out_regs.insert(d.num); }
            return;
        }
        if (d.type == RT_INPUT) {
            if (major >= 3) in_dcl.push_back({usage, uidx, (uint8_t)m, fmt("v%d", d.num)});
            else in_dcl.push_back({10, d.num, (uint8_t)m, fmt("v%d", d.num)});    // SM2 color input
            in_regs.insert(d.num);
        } else if (d.type == RT_ADDR) {    // SM2 texcoord input t#
            in_dcl.push_back({5, d.num, (uint8_t)m, fmt("t%d", d.num)});
            t_regs.insert(d.num);
        } else if (d.type == RT_MISC) {
            if (d.num == 0) S.uses_vpos = true; else S.uses_vface = true;
        }
    }

    // ---------------------------------------------------------------- texture sampling
    std::string coord(int sampler, const std::string &c) {
        int ty = sampler < 16 ? S.sampler_type[sampler] : 2;
        return ty == 3 || ty == 4 ? "(" + c + ").xyz" : "(" + c + ").xy";
    }

    // ---------------------------------------------------------------- one instruction
    void insn() {
        uint32_t tok = t[pos];
        uint32_t op = tok & 0xFFFF;
        if (op == OP_COMMENT) { pos += 1 + ((tok >> 16) & 0x7FFF); return; }
        if (op == OP_END) { pos = n; return; }
        size_t len = (tok >> 24) & 0xF;
        size_t next = pos + 1 + len;
        pos++;
        bool predicated = tok & (1u << 28);
        uint32_t ctrl = (tok >> 16) & 0xFF;
        auto P = [&](bool s = true) { return read_param(s); };

        switch (op) {
        case OP_NOP: break;
        case OP_DCL: do_dcl(); break;
        case OP_DEF: {
            Param d = P(false);
            std::array<float, 4> v;
            memcpy(v.data(), &t[pos], 16);
            S.fdefs.push_back({d.num, v});
            break;
        }
        case OP_DEFI: {
            Param d = P(false);
            std::array<int, 4> v;
            memcpy(v.data(), &t[pos], 16);
            S.idefs.push_back({d.num, v});
            break;
        }
        case OP_DEFB: { Param d = P(false); S.bdefs.push_back({d.num, (int)t[pos]}); break; }

        // ---- arithmetic
        case OP_MOV: case OP_MOVA: case OP_ADD: case OP_SUB: case OP_MUL: case OP_MAD: case OP_MIN: case OP_MAX:
        case OP_SLT: case OP_SGE: case OP_FRC: case OP_ABS: case OP_SGN: case OP_LRP: case OP_CMP: case OP_CND:
        case OP_DP3: case OP_DP4: case OP_DP2ADD: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_EXPP: case OP_LOG:
        case OP_LOGP: case OP_POW: case OP_NRM: case OP_CRS: case OP_LIT: case OP_DST: case OP_DSX: case OP_DSY:
        case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: case OP_SINCOS: {
            Param d = P(false);
            int nsrc = (int)len - 1 - (d.has_rel && major >= 2 ? 1 : 0) - (predicated ? 1 : 0);
            Param s[4];
            int ns = 0;
            size_t end_src = next - (predicated ? 1 : 0);
            while (pos < end_src && ns < 4) s[ns++] = P();
            (void)nsrc;
            Param pr;
            if (predicated && pos < next) pr = P();
            std::string e;
            auto S0 = [&] { return src(s[0]); };
            auto S1 = [&] { return src(s[1]); };
            auto S2 = [&] { return src(s[2]); };
            switch (op) {
            case OP_MOV: case OP_MOVA: e = S0(); break;
            case OP_ADD: e = S0() + " + " + S1(); break;
            case OP_SUB: e = S0() + " - " + S1(); break;
            case OP_MUL: e = S0() + " * " + S1(); break;
            case OP_MAD: e = S0() + " * " + S1() + " + " + S2(); break;
            case OP_MIN: e = "min(" + S0() + ", " + S1() + ")"; break;
            case OP_MAX: e = "max(" + S0() + ", " + S1() + ")"; break;
            case OP_SLT: e = "vec4(lessThan(" + S0() + ", " + S1() + "))"; break;
            case OP_SGE: e = "vec4(greaterThanEqual(" + S0() + ", " + S1() + "))"; break;
            case OP_FRC: e = "fract(" + S0() + ")"; break;
            case OP_ABS: e = "abs(" + S0() + ")"; break;
            case OP_SGN: e = "sign(" + S0() + ")"; break;
            case OP_LRP: e = "mix(" + S2() + ", " + S1() + ", " + S0() + ")"; break;
            case OP_CMP: e = "cmp4(" + S0() + ", " + S1() + ", " + S2() + ")"; break;
            case OP_CND: e = "cnd4(" + S0() + ", " + S1() + ", " + S2() + ")"; break;
            case OP_DP3: e = "vec4(dot((" + S0() + ").xyz, (" + S1() + ").xyz))"; break;
            case OP_DP4: e = "vec4(dot(" + S0() + ", " + S1() + "))"; break;
            case OP_DP2ADD: e = "vec4(dot((" + S0() + ").xy, (" + S1() + ").xy) + (" + S2() + ").x)"; break;
            case OP_RCP: e = "vec4(rcp1(" + srcx(s[0]) + "))"; break;
            case OP_RSQ: e = "vec4(rsq1(" + srcx(s[0]) + "))"; break;
            case OP_EXP: case OP_EXPP: e = "vec4(exp2(" + srcx(s[0]) + "))"; break;
            case OP_LOG: case OP_LOGP: e = "vec4(log1(" + srcx(s[0]) + "))"; break;
            case OP_POW: e = "vec4(pow1(" + srcx(s[0]) + ", " + srcx(s[1]) + "))"; break;
            case OP_NRM: e = "nrm4(" + S0() + ")"; break;
            case OP_CRS: e = "vec4(cross((" + S0() + ").xyz, (" + S1() + ").xyz), 0.0)"; break;
            case OP_LIT: e = "lit4(" + S0() + ")"; break;
            case OP_DST: e = "vec4(1.0, (" + S0() + ").y * (" + S1() + ").y, (" + S0() + ").z, (" + S1() + ").w)"; break;
            case OP_DSX: e = "dFdx(" + S0() + ")"; break;
            case OP_DSY: e = "dFdy(" + S0() + ")"; break;
            case OP_SINCOS: e = "vec4(cos(" + srcx(s[0]) + "), sin(" + srcx(s[0]) + "), 0.0, 0.0)"; break;
            case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
                int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M3x2 ? 2 : 3;
                bool three = op == OP_M3x4 || op == OP_M3x3 || op == OP_M3x2;
                std::string a = S0();
                e = "vec4(";
                for (int k = 0; k < 4; k++) {
                    if (k) e += ", ";
                    if (k >= rows) { e += "0.0"; continue; }
                    std::string r = row(s[1], k);
                    e += three ? "dot((" + a + ").xyz, (" + r + ").xyz)" : "dot(" + a + ", " + r + ")";
                }
                e += ")";
                break;
            }
            }
            assign(d, e, predicated ? &pr : nullptr);
            break;
        }

        case OP_SETP: {
            Param d = P(false), a = P(), b = P();
            uint32_t m = (d.tok >> 16) & 0xF;
            uses_p0 = true;
            const char *fn[] = {"?", "greaterThan", "equal", "greaterThanEqual", "lessThan", "notEqual", "lessThanEqual", "?"};
            std::string e = std::string(fn[ctrl & 7]) + "(" + src(a) + ", " + src(b) + ")";
            if (m == 0xF) line("p0 = " + e + ";");
            else { std::string ms = mask_str(m); line("p0." + ms + " = (" + e + ")." + ms + ";"); }
            break;
        }

        // ---- texturing
        case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
            if (major < 2) { fail("ps_1_x texture ops"); break; }
            Param d = P(false), c = P(), s = P();
            Param dx, dy;
            if (op == OP_TEXLDD) { dx = P(); dy = P(); }
            Param pr;
            if (predicated) pr = P();
            int sn = s.num;
            if (sn < 16 && !(S.samplers_used & (1 << sn))) { S.samplers_used |= 1 << sn; S.sampler_type[sn] = 2; }
            std::string sname = reg_name(s, false);
            std::string cs = src(c);
            int ty = sn < 16 ? S.sampler_type[sn] : 2;
            std::string e;
            if (op == OP_TEXLDL) e = "textureLod(" + sname + ", " + coord(sn, cs) + ", (" + cs + ").w)";
            else if (op == OP_TEXLDD) e = "textureGrad(" + sname + ", " + coord(sn, cs) + ", " + coord(sn, src(dx)) + ", " + coord(sn, src(dy)) + ")";
            else if (ctrl & 1) {   // texldp
                if (ty == 2) e = "textureProj(" + sname + ", (" + cs + ").xyw)";
                else e = "texture(" + sname + ", (" + cs + ").xyz / (" + cs + ").w)";
            } else if (ctrl & 2) e = "texture(" + sname + ", " + coord(sn, cs) + ", (" + cs + ").w)";
            else if (!ps) e = "textureLod(" + sname + ", " + coord(sn, cs) + ", 0.0)";
            else e = "texture(" + sname + ", " + coord(sn, cs) + ")";
            // undo the swizzle on the sampler register (D3D allows .swizzle on s#)
            uint32_t ssw = (s.tok >> 16) & 0xFF;
            if (ssw != 0xE4) { e = "(" + e + ")."; for (int i = 0; i < 4; i++) e += "xyzw"[(ssw >> (2 * i)) & 3]; }
            assign(d, e, predicated ? &pr : nullptr);
            break;
        }
        case OP_TEXKILL: {
            Param d = P(false);
            uint32_t m = (d.tok >> 16) & 0xF;
            std::string ms = mask_str(m ? m : 7);
            if (ms.size() == 1) line("if (" + reg_name(d, false) + "." + ms + " < 0.0) discard;");
            else line("if (any(lessThan(" + reg_name(d, false) + "." + ms + ", vec" + std::to_string(ms.size()) + "(0.0)))) discard;");
            break;
        }

        // ---- flow control
        case OP_IF: {
            Param b = P();
            if (b.type == RT_PRED) line("if (" + pred(b) + ") {");
            else line("if (" + reg_name(b, false) + ") {");
            indent++;
            break;
        }
        case OP_IFC: {
            Param a = P(), b = P();
            line("if (" + srcx(a) + " " + CMPOP[ctrl & 7] + " " + srcx(b) + ") {");
            indent++;
            break;
        }
        case OP_ELSE: indent--; line("} else {"); indent++; break;
        case OP_ENDIF: indent--; line("}"); break;
        case OP_REP: {
            Param i = P();
            int k = rep_count++;
            line(fmt("for (int rep%d = 0; rep%d < %s.x; rep%d++) {", k, k, reg_name(i, false).c_str(), k));
            indent++;
            loop_kind.push_back(0);
            break;
        }
        case OP_ENDREP: indent--; line("}"); if (!loop_kind.empty()) loop_kind.pop_back(); break;
        case OP_LOOP: {
            Param al = P(), i = P();
            (void)al;
            uses_aL = true;
            int k = rep_count++;
            std::string in = reg_name(i, false);
            line(fmt("int aLsave%d = aL;", k));
            line(fmt("aL = %s.y;", in.c_str()));
            line(fmt("for (int rep%d = 0; rep%d < %s.x; rep%d++, aL += %s.z) {", k, k, in.c_str(), k, in.c_str()));
            indent++;
            loop_kind.push_back(1 + k);
            break;
        }
        case OP_ENDLOOP: {
            indent--; line("}");
            if (!loop_kind.empty()) { int k = loop_kind.back() - 1; loop_kind.pop_back(); if (k >= 0) line(fmt("aL = aLsave%d;", k)); }
            break;
        }
        case OP_BREAK: line("break;"); break;
        case OP_BREAKC: {
            Param a = P(), b = P();
            line("if (" + srcx(a) + " " + CMPOP[ctrl & 7] + " " + srcx(b) + ") break;");
            break;
        }
        case OP_BREAKP: { Param b = P(); line("if (" + pred(b) + ") break;"); break; }
        case OP_LABEL: {
            Param l = P();
            if (out == &main_body) { /* main ended by ret */ }
            out = &funcs;
            indent = 1;
            funcs += fmt("void L%d() {\n", l.num);
            in_label = true;
            break;
        }
        case OP_RET:
            if (in_label) { if (indent == 1) { funcs += "}\n"; in_label = false; } else line("return;"); }
            else if (indent > 1) fail("return inside main flow control");
            break;
        case OP_CALL: { Param l = P(); labels_called.insert(l.num); line(fmt("L%d();", l.num)); break; }
        case OP_CALLNZ: {
            Param l = P(), b = P();
            labels_called.insert(l.num);
            if (b.type == RT_PRED) line(fmt("if (%s) L%d();", pred(b).c_str(), l.num));
            else line(fmt("if (%s) L%d();", reg_name(b, false).c_str(), l.num));
            break;
        }
        case OP_PHASE: break;
        default:
            fail(fmt("opcode %u", op));
            break;
        }
        pos = next;
    }

    bool run() {
        if (n < 1) { S.error = "empty"; return false; }
        uint32_t v = t[0];
        if ((v >> 16) == 0xFFFF) ps = true;
        else if ((v >> 16) != 0xFFFE) { S.error = "bad version token"; return false; }
        major = (v >> 8) & 0xFF; minor = v & 0xFF;
        S.ps = ps; S.major = major; S.minor = minor;
        if (major < 2) { S.error = "shader model 1.x not supported"; return false; }
        pos = 1;
        while (pos < n && ok) insn();
        if (!ok) return false;
        if (in_label) funcs += "}\n";
        build();
        return true;
    }

    void build() {
        std::string &D = S.decls;
        for (int i = 0; i <= max_temp; i++) D += fmt("vec4 r%d;\n", i);
        if (uses_a0) D += "ivec4 a0;\n";
        if (uses_aL) D += "int aL;\n";
        if (uses_p0) D += "bvec4 p0;\n";
        if (!ps) {
            for (int r : in_regs) {
                auto it = vs_in.find(r);
                if (it != vs_in.end()) S.inputs.push_back(it->second);
                else S.inputs.push_back({0, 0, 15, fmt("v%d", r)});
            }
            if (major >= 3) {
                for (int r : out_regs) D += fmt("vec4 o%d;\n", r);
                for (auto &o : out_dcl) {
                    if (o.usage == 0 && o.index == 0) { S.pos_reg = o.reg; continue; }
                    if (o.usage == 4) { S.writes_psize = true; S.psize_reg = o.reg + "." + mask_str(o.mask).substr(0, 1); continue; }
                    S.outputs.push_back(o);
                }
            } else {
                D += "vec4 o_pos;\n";
                for (int r : od_regs) { D += fmt("vec4 oD%d;\n", r); S.outputs.push_back({10, r, 15, fmt("oD%d", r)}); }
                for (int r : ot_regs) { D += fmt("vec4 oT%d;\n", r); S.outputs.push_back({5, r, 15, fmt("oT%d", r)}); }
                if (uses_ofog) { D += "vec4 o_fog;\n"; S.outputs.push_back({11, 0, 1, "o_fog"}); S.writes_fog = true; }
                if (uses_opts) { D += "vec4 o_pts;\n"; S.writes_psize = true; }
            }
            if (!uses_opos && major < 3) {}
        } else {
            for (int r : in_regs) D += fmt("vec4 v%d;\n", r);
            for (int r : t_regs) D += fmt("vec4 t%d;\n", r);
            S.inputs = in_dcl;
            // inputs used but never declared (SM2 allows implicit use)
            std::set<std::string> have;
            for (auto &i : in_dcl) have.insert(i.reg);
            for (int r : in_regs) if (!have.count(fmt("v%d", r)) && major < 3) S.inputs.push_back({10, r, 15, fmt("v%d", r)});
            for (int r : t_regs) if (!have.count(fmt("t%d", r))) S.inputs.push_back({5, r, 15, fmt("t%d", r)});
            if (uses_odepth) { D += "vec4 o_depth;\n"; S.writes_depth = true; }
            int mc = 0;
            for (int c : color_outs) if (c + 1 > mc) mc = c + 1;
            S.color_outputs = mc ? mc : 1;
            if (S.uses_vpos) D += "vec4 vpos;\n";
            if (S.uses_vface) D += "float vface;\n";
        }
        for (int l : labels_called) D += fmt("void L%d();\n", l);
        S.body = main_body;
        S.funcs = funcs;
    }
};

const char *PRELUDE =
    "vec4 cmp4(vec4 a, vec4 b, vec4 c) { return vec4(a.x >= 0.0 ? b.x : c.x, a.y >= 0.0 ? b.y : c.y, a.z >= 0.0 ? b.z : c.z, a.w >= 0.0 ? b.w : c.w); }\n"
    "vec4 cnd4(vec4 a, vec4 b, vec4 c) { return vec4(a.x > 0.5 ? b.x : c.x, a.y > 0.5 ? b.y : c.y, a.z > 0.5 ? b.z : c.z, a.w > 0.5 ? b.w : c.w); }\n"
    "float rcp1(float x) { return x == 0.0 ? 3.0e38 : 1.0 / x; }\n"
    "float rsq1(float x) { float a = abs(x); return a == 0.0 ? 3.0e38 : inversesqrt(a); }\n"
    "float log1(float x) { float a = abs(x); return a == 0.0 ? -3.0e38 : log2(a); }\n"
    "float pow1(float x, float y) { float a = abs(x); return (a == 0.0 && y <= 0.0) ? (y == 0.0 ? 1.0 : 3.0e38) : pow(a, y); }\n"
    "vec4 nrm4(vec4 v) { float d = dot(v.xyz, v.xyz); return d > 0.0 ? v * inversesqrt(d) : vec4(0.0); }\n"
    "vec4 lit4(vec4 s) { vec4 r = vec4(1.0, 0.0, 0.0, 1.0); if (s.x > 0.0) { r.y = s.x; if (s.y > 0.0) r.z = pow(s.y, clamp(s.w, -127.9961, 127.9961)); } return r; }\n";

static const char *sampler_glsl(uint32_t ty) {
    return ty == 3 ? "samplerCube" : ty == 4 ? "sampler3D" : "sampler2D";
}

} // namespace

std::string sm_varying(int usage, int index) { return fmt("V_%d_%d", usage, index); }

size_t sm_length(const uint32_t *t, size_t max) {
    if (!max) return 0;
    uint32_t v = t[0];
    if ((v >> 16) != 0xFFFF && (v >> 16) != 0xFFFE) return 0;
    size_t p = 1;
    while (p < max) {
        uint32_t tok = t[p];
        if (tok == 0x0000FFFF) return p + 1;
        if ((tok & 0xFFFF) == OP_COMMENT) p += 1 + ((tok >> 16) & 0x7FFF);
        else if (((v >> 8) & 0xFF) >= 2) p += 1 + ((tok >> 24) & 0xF);
        else return 0;
    }
    return 0;
}

bool sm_translate(const uint32_t *tokens, size_t max_tokens, SmShader &out) {
    size_t len = sm_length(tokens, max_tokens);
    if (!len) { out.error = "cannot determine shader length"; return false; }
    Tr tr(tokens, len, out);
    return tr.run();
}

static std::string comps(int n) { return std::string("xyzw").substr(0, n); }

std::string sm_vs_source(const SmShader &vs, const SmShader *ps) {
    std::string s = "#version 410 core\n";
    s += PRELUDE;
    s += "uniform vec4 vc[256];\nuniform ivec4 vi[16];\nuniform bool vb[16];\nuniform vec4 posFixup;\n";
    for (int i = 0; i < 4; i++)
        if (vs.samplers_used & (1 << i)) s += fmt("uniform %s vs_s%d;\n", sampler_glsl(vs.sampler_type[i]), i);
    for (auto &in : vs.inputs) s += fmt("layout(location = %s) in vec4 %s;\n", in.reg.c_str() + 1, in.reg.c_str());
    std::set<std::string> written;
    for (auto &o : vs.outputs) {
        std::string v = sm_varying(o.usage, o.index);
        if (written.insert(v).second) s += "out vec4 " + v + ";\n";
    }
    std::vector<std::string> extra;
    if (ps) {
        std::vector<std::string> need;
        for (auto &i : ps->inputs) need.push_back(sm_varying(i.usage, i.index));
        if (ps->major < 3) need.push_back(sm_varying(11, 0));
        for (auto &v : need)
            if (written.insert(v).second) { s += "out vec4 " + v + ";\n"; extra.push_back(v); }
    }
    s += vs.decls;
    s += vs.funcs;
    s += "void main() {\n";
    // D3D leaves temps undefined; zero them for determinism
    if (vs.major >= 3) {
        for (size_t p = 0; (p = vs.decls.find("vec4 o", p)) != std::string::npos; p++) {
            size_t e = vs.decls.find(';', p);
            s += "    " + vs.decls.substr(p + 5, e - p - 5) + " = vec4(0.0);\n";
        }
    } else {
        s += "    o_pos = vec4(0.0);\n";
        for (auto &o : vs.outputs) s += "    " + o.reg + " = vec4(0.0);\n";
        if (vs.writes_psize) s += "    o_pts = vec4(1.0);\n";
    }
    s += vs.body;
    // epilogue: semantics -> varyings
    std::set<std::string> assigned;
    for (auto &o : vs.outputs) {
        std::string v = sm_varying(o.usage, o.index);
        if (assigned.insert(v).second) s += "    " + v + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
        int nc = popcnt4(o.mask);
        if (o.usage == 11 && vs.major < 3) s += "    " + v + ".x = o_fog.x;\n";
        else if (nc == 4) s += "    " + v + " = " + o.reg + ";\n";
        else s += "    " + v + "." + comps(nc) + " = " + o.reg + "." + mask_str(o.mask) + ";\n";
    }
    for (auto &v : extra) s += "    " + v + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
    std::string pos = "o_pos";
    if (vs.major >= 3) {
        pos = vs.pos_reg.empty() ? "vec4(0.0, 0.0, 0.0, 1.0)" : vs.pos_reg;
        if (vs.writes_psize) s += "    gl_PointSize = " + vs.psize_reg + ";\n";
    }
    s += "    gl_Position = " + pos + ";\n";
    s += "    gl_Position.y = -gl_Position.y;\n";
    s += "    gl_Position.xy += posFixup.xy * gl_Position.w;\n";
    s += "    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n";
    if (vs.writes_psize && vs.major < 3) s += "    gl_PointSize = o_pts.x;\n";
    s += "}\n";
    return s;
}

std::string sm_ps_source(const SmShader &ps, const SmShader *vs) {
    (void)vs;
    std::string s = "#version 410 core\n";
    s += PRELUDE;
    s += "uniform vec4 pc[256];\nuniform ivec4 pi[16];\nuniform bool pb[16];\n";
    s += "uniform int alphaFunc;\nuniform float alphaRef;\nuniform float vfaceSign;\n";
    s += "uniform vec4 fogColor;\nuniform vec4 fogParams;\n";   // x: mode (0 off), y: start, z: end, w: density
    for (int i = 0; i < 16; i++)
        if (ps.samplers_used & (1 << i)) s += fmt("uniform %s ps_s%d;\n", sampler_glsl(ps.sampler_type[i]), i);
    std::set<std::string> declared;
    for (auto &i : ps.inputs) {
        std::string v = sm_varying(i.usage, i.index);
        if (declared.insert(v).second) s += "in vec4 " + v + ";\n";
    }
    bool fog_in = ps.major < 3 && !declared.count(sm_varying(11, 0));
    if (fog_in) s += "in vec4 " + sm_varying(11, 0) + ";\n";
    for (int i = 0; i < ps.color_outputs; i++) s += fmt("layout(location = %d) out vec4 oC%d;\n", i, i);
    s += ps.decls;
    s += ps.funcs;
    s += "bool atest(float a) {\n"
         "    float r = alphaRef;\n"
         "    if (alphaFunc == 1) return false;\n    if (alphaFunc == 2) return a < r;\n"
         "    if (alphaFunc == 3) return abs(a - r) < 0.5 / 255.0;\n    if (alphaFunc == 4) return a <= r;\n"
         "    if (alphaFunc == 5) return a > r;\n    if (alphaFunc == 6) return abs(a - r) >= 0.5 / 255.0;\n"
         "    if (alphaFunc == 7) return a >= r;\n    return true;\n}\n";
    s += "void main() {\n";
    for (int i = 0; i < ps.color_outputs; i++) s += fmt("    oC%d = vec4(0.0);\n", i);
    for (size_t p = 0; (p = ps.decls.find("vec4 r", p)) != std::string::npos; p++) {
        size_t e = ps.decls.find(';', p);
        s += "    " + ps.decls.substr(p + 5, e - p - 5) + " = vec4(0.0);\n";
    }
    std::set<std::string> inited;
    for (auto &i : ps.inputs) {
        std::string v = sm_varying(i.usage, i.index);
        if (inited.insert(i.reg).second) s += "    " + i.reg + " = vec4(0.0);\n";
        int nc = popcnt4(i.mask ? i.mask : 15);
        if (nc == 4) s += "    " + i.reg + " = " + v + ";\n";
        else s += "    " + i.reg + "." + mask_str(i.mask) + " = " + v + "." + comps(nc) + ";\n";
    }
    if (ps.uses_vpos) s += "    vpos = vec4(gl_FragCoord.xy - 0.5, 0.0, 1.0);\n";
    if (ps.uses_vface) s += "    vface = gl_FrontFacing ? vfaceSign : -vfaceSign;\n";
    s += ps.body;
    // fixed-function fog (only applies to ps_2_x in D3D9)
    if (ps.major < 3)
        s += "    if (fogParams.x > 0.0) { float f = clamp(" + sm_varying(11, 0) + ".x, 0.0, 1.0); oC0.rgb = mix(fogColor.rgb, oC0.rgb, f); }\n";
    s += "    if (!atest(oC0.a)) discard;\n";
    if (ps.writes_depth) s += "    gl_FragDepth = clamp(o_depth.x, 0.0, 1.0);\n";
    s += "}\n";
    return s;
}
