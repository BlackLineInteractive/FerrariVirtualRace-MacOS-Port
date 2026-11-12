"""x87, SSE/SSE2 and MMX translation rules for recomp.py."""
from capstone import x86_const as X

CF, ZF, SF, OF, PF, AF = 1, 2, 4, 8, 16, 32


class Unsupported(Exception):
    pass


def xr(insn, op):
    return insn.reg_name(op.reg)


class FpuSse:
    def translate(self, ft, insn):
        b = insn.bytes
        i = 0
        while i < len(b) and b[i] in (0x9B, 0x64, 0x65, 0x2E, 0x3E, 0x26, 0x36, 0xF0):
            i += 1
        if i < len(b) and 0xD8 <= b[i] <= 0xDF:
            return self.x87(ft, insn, b[i], b[i + 1])
        m = insn.mnemonic
        if m in ("fwait", "wait"):
            return []
        return self.sse(ft, insn)

    # ------------------------------------------------------------------ x87
    def x87(self, ft, insn, op, modrm):
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        a = insn.address
        if mod != 3:
            mops = [o for o in insn.operands if o.type == X.X86_OP_MEM]
            if not mops:
                raise Unsupported("x87 mem operand")
            ad = ft.addr(insn, mops[0])
            return self.x87_mem(op, reg, ad, a)
        return self.x87_reg(op, reg, rm, modrm, a)

    @staticmethod
    def arith(reg, dst, src, mem=False):
        # reg field semantic for D8/DA/DC/DE (memory) and D8 (register): dst = dst op src
        return {0: f"{dst} = {dst} + {src};", 1: f"{dst} = {dst} * {src};",
                4: f"{dst} = {dst} - {src};", 5: f"{dst} = {src} - {dst};",
                6: f"{dst} = {dst} / {src};", 7: f"{dst} = {src} / {dst};"}[reg]

    @staticmethod
    def fcom(x, y, pop):
        s = [f"{{ double x_ = {x}, y_ = {y}; c->fsw &= ~0x4500; "
             "if (x_ != x_ || y_ != y_) c->fsw |= 0x4500; else if (x_ < y_) c->fsw |= 0x0100; else if (x_ == y_) c->fsw |= 0x4000; }"]
        s += ["FPOP(c);"] * pop
        return s

    @staticmethod
    def fcomi(x, y, pop):
        s = [f"{{ double x_ = {x}, y_ = {y}; c->of = c->sf = c->af = 0; "
             "if (x_ != x_ || y_ != y_) { c->zf = c->pf = c->cf = 1; } else { c->pf = 0; c->zf = x_ == y_; c->cf = x_ < y_; } }"]
        s += ["FPOP(c);"] * pop
        return s

    def x87_mem(self, op, reg, ad, a):
        if op in (0xD8, 0xDC, 0xDA, 0xDE):
            src = {0xD8: f"(double)RF({ad})", 0xDC: f"RD({ad})",
                   0xDA: f"(double)(int32_t)R32({ad})", 0xDE: f"(double)(int16_t)R16({ad})"}[op]
            if reg in (2, 3):
                return self.fcom("ST(0)", src, 1 if reg == 3 else 0)
            return [f"{{ double s_ = {src}; " + self.arith(reg, "ST(0)", "s_") + " }"]
        if op == 0xD9:
            if reg == 0: return [f"FPUSH(c, (double)RF({ad}));"]
            if reg == 2: return [f"WF({ad}, (float)ST(0));"]
            if reg == 3: return [f"WF({ad}, (float)FPOP(c));"]
            if reg == 5: return [f"c->fcw = R16({ad});"]
            if reg == 7: return [f"W16({ad}, c->fcw);"]
            if reg == 6: return [f"rt_fnstenv(c, {ad});"]
            if reg == 4: return [f"rt_fldenv(c, {ad});"]
        if op == 0xDB:
            if reg == 0: return [f"FPUSH(c, (double)(int32_t)R32({ad}));"]
            if reg == 1: return [f"W32({ad}, (uint32_t)rt_fist(c, FPOP(c), 3, 32));"]
            if reg == 2: return [f"W32({ad}, (uint32_t)rt_fist(c, ST(0), -1, 32));"]
            if reg == 3: return [f"W32({ad}, (uint32_t)rt_fist(c, FPOP(c), -1, 32));"]
            if reg == 5: return [f"FPUSH(c, rt_load80({ad}));"]
            if reg == 7: return [f"rt_store80({ad}, FPOP(c));"]
        if op == 0xDD:
            if reg == 0: return [f"FPUSH(c, RD({ad}));"]
            if reg == 1: return [f"W64({ad}, (uint64_t)rt_fist(c, FPOP(c), 3, 64));"]
            if reg == 2: return [f"WD({ad}, ST(0));"]
            if reg == 3: return [f"WD({ad}, FPOP(c));"]
            if reg == 4: return [f"rt_frstor(c, {ad});"]
            if reg == 6: return [f"rt_fnsave(c, {ad});"]
            if reg == 7: return [f"W16({ad}, (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)));"]
        if op == 0xDF:
            if reg == 0: return [f"FPUSH(c, (double)(int16_t)R16({ad}));"]
            if reg == 1: return [f"W16({ad}, (uint16_t)rt_fist(c, FPOP(c), 3, 16));"]
            if reg == 2: return [f"W16({ad}, (uint16_t)rt_fist(c, ST(0), -1, 16));"]
            if reg == 3: return [f"W16({ad}, (uint16_t)rt_fist(c, FPOP(c), -1, 16));"]
            if reg == 5: return [f"FPUSH(c, (double)(int64_t)R64({ad}));"]
            if reg == 7: return [f"W64({ad}, (uint64_t)rt_fist(c, FPOP(c), -1, 64));"]
        raise Unsupported(f"x87 mem {op:02x}/{reg}")

    def x87_reg(self, op, reg, rm, modrm, a):
        sti = f"ST({rm})"
        if op == 0xD8:
            if reg in (2, 3):
                return self.fcom("ST(0)", sti, 1 if reg == 3 else 0)
            return [f"{{ double s_ = {sti}; " + self.arith(reg, "ST(0)", "s_") + " }"]
        if op in (0xDC, 0xDE):
            # dst = st(i); note reversed sub/div encoding relative to D8
            pop = op == 0xDE
            if op == 0xDE and modrm == 0xD9:
                return self.fcom("ST(0)", "ST(1)", 2)
            sem = {0: f"{sti} = {sti} + ST(0);", 1: f"{sti} = {sti} * ST(0);",
                   4: f"{sti} = ST(0) - {sti};", 5: f"{sti} = {sti} - ST(0);",
                   6: f"{sti} = ST(0) / {sti};", 7: f"{sti} = {sti} / ST(0);"}.get(reg)
            if sem is None:
                if reg in (2, 3):
                    return self.fcom("ST(0)", sti, 1 if reg == 3 else 0)
                raise Unsupported("dc/de reg")
            return [sem] + (["FPOP(c);"] if pop else [])
        if op == 0xD9:
            if reg == 0: return [f"{{ double v_ = {sti}; FPUSH(c, v_); }}"]
            if reg == 1: return [f"{{ double t_ = ST(0); ST(0) = {sti}; {sti} = t_; }}"]
            simple = {
                0xD0: [], 0xE0: ["ST(0) = -ST(0);"], 0xE1: ["ST(0) = fabs(ST(0));"],
                0xE4: self.fcom("ST(0)", "0.0", 0), 0xE5: ["rt_fxam(c);"],
                0xE8: ["FPUSH(c, 1.0);"], 0xE9: ["FPUSH(c, 3.321928094887362347870319429489390175864831393);"],
                0xEA: ["FPUSH(c, 1.442695040888963407359924681001892137426645954);"],
                0xEB: ["FPUSH(c, M_PI);"], 0xEC: ["FPUSH(c, 0.301029995663981195213738894724493026768189881);"],
                0xED: ["FPUSH(c, M_LN2);"], 0xEE: ["FPUSH(c, 0.0);"],
                0xF0: ["ST(0) = exp2(ST(0)) - 1.0;"], 0xF1: ["{ double x_ = FPOP(c); ST(0) = ST(0) * log2(x_); }"],
                0xF2: ["ST(0) = tan(ST(0)); FPUSH(c, 1.0); c->fsw &= ~0x0400;"],
                0xF3: ["{ double x_ = FPOP(c); ST(0) = atan2(ST(0), x_); }"],
                0xF5: ["ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x0400;"],
                0xF6: ["c->ftop = (c->ftop - 1) & 7;"], 0xF7: ["c->ftop = (c->ftop + 1) & 7;"],
                0xF8: ["ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x0400;"],
                0xF9: ["{ double x_ = FPOP(c); ST(0) = ST(0) * log2(x_ + 1.0); }"],
                0xFA: ["ST(0) = sqrt(ST(0));"],
                0xFB: ["{ double x_ = ST(0); ST(0) = sin(x_); FPUSH(c, cos(x_)); c->fsw &= ~0x0400; }"],
                0xFC: ["ST(0) = rt_round(c, ST(0));"], 0xFD: ["ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"],
                0xFE: ["ST(0) = sin(ST(0)); c->fsw &= ~0x0400;"], 0xFF: ["ST(0) = cos(ST(0)); c->fsw &= ~0x0400;"],
            }
            if modrm in simple:
                return simple[modrm]
        if op == 0xDA:
            if modrm == 0xE9:
                return self.fcom("ST(0)", "ST(1)", 2)
            cond = {0: "c->cf", 1: "c->zf", 2: "(c->cf|c->zf)", 3: "c->pf"}.get(reg)
            if cond: return [f"if ({cond}) ST(0) = {sti};"]
        if op == 0xDB:
            if modrm in (0xE2, 0xE0, 0xE1, 0xE4): return ["c->fsw &= 0x7f00;" if modrm == 0xE2 else ""]
            if modrm == 0xE3: return ["c->fcw = 0x37f; c->fsw = 0; c->ftop = 0;"]
            if reg in (5, 6): return self.fcomi("ST(0)", sti, 0)
            cond = {0: "!c->cf", 1: "!c->zf", 2: "!(c->cf|c->zf)", 3: "!c->pf"}.get(reg)
            if cond: return [f"if ({cond}) ST(0) = {sti};"]
        if op == 0xDD:
            if reg == 0: return []  # ffree
            if reg == 2: return [f"{sti} = ST(0);"]
            if reg == 3: return [f"{{ double v_ = ST(0); {sti} = v_; FPOP(c); }}"]
            if reg in (4, 5): return self.fcom("ST(0)", sti, 1 if reg == 5 else 0)
        if op == 0xDF:
            if modrm == 0xE0: return ["c->eax = (c->eax & 0xffff0000u) | (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11));"]
            if reg in (5, 6): return self.fcomi("ST(0)", sti, 1)
        raise Unsupported(f"x87 reg {op:02x} {modrm:02x}")

    # ------------------------------------------------------------------ SSE / MMX
    def vec(self, ft, insn, op):
        """C lvalue-ish pointer expression to a 16-byte XMM or memory location."""
        if op.type == X.X86_OP_REG:
            n = xr(insn, op)
            if n.startswith("xmm"):
                return f"(&c->xmm[{n[3:]}])", True
            if n.startswith("mm"):
                return f"((XMM*)&c->mm[{n[2:]}])", True
            raise Unsupported(f"vec reg {n}")
        return f"((XMM*)H({ft.addr(insn, op)}))", False

    def sse(self, ft, insn):
        m = insn.mnemonic
        ops = insn.operands
        live = ft.live_out.get(insn.address, 0)
        if not ops:
            raise Unsupported("no operands")

        def V(k):
            return self.vec(ft, insn, ops[k])[0]

        def isreg(k):
            return ops[k].type == X.X86_OP_REG

        def gp(k):
            return ops[k].type == X.X86_OP_REG and not xr(insn, ops[k]).startswith(("xmm", "mm"))

        def lane_mem(k, typ):
            if isreg(k):
                return f"{V(k)}->{typ}[0]"
            ad = ft.addr(insn, ops[k])
            return {"f": f"RF({ad})", "d": f"RD({ad})", "u": f"R32({ad})", "q": f"R64({ad})"}[typ]

        def store_lane(k, typ, v):
            if isreg(k):
                return f"{V(k)}->{typ}[0] = {v};"
            ad = ft.addr(insn, ops[k])
            return {"f": f"WF({ad}, {v});", "d": f"WD({ad}, {v});", "u": f"W32({ad}, {v});", "q": f"W64({ad}, {v});"}[typ]

        # ---- moves
        if m in ("movaps", "movups", "movapd", "movupd", "movdqa", "movdqu"):
            return [f"memcpy({V(0)}, {V(1)}, 16);"]
        if m == "movss":
            if isreg(0) and isreg(1):
                return [f"{V(0)}->f[0] = {V(1)}->f[0];"]
            if isreg(0):
                return [f"{{ float v_ = {lane_mem(1, 'f')}; XMM *d_ = {V(0)}; d_->q[0] = d_->q[1] = 0; d_->f[0] = v_; }}"]
            return [store_lane(0, "f", f"{V(1)}->f[0]")]
        if m == "movsd":
            if isreg(0) and isreg(1):
                return [f"{V(0)}->d[0] = {V(1)}->d[0];"]
            if isreg(0):
                return [f"{{ double v_ = {lane_mem(1, 'd')}; XMM *d_ = {V(0)}; d_->q[1] = 0; d_->d[0] = v_; }}"]
            return [store_lane(0, "d", f"{V(1)}->d[0]")]
        if m in ("movlpd", "movlps"):
            if isreg(0):
                return [f"{V(0)}->q[0] = {lane_mem(1, 'q')};"]
            return [store_lane(0, "q", f"{V(1)}->q[0]")]
        if m in ("movhpd", "movhps"):
            if isreg(0):
                return [f"{V(0)}->q[1] = R64({ft.addr(insn, ops[1])});"]
            return [f"W64({ft.addr(insn, ops[0])}, {V(1)}->q[1]);"]
        if m == "movhlps":
            return [f"{V(0)}->q[0] = {V(1)}->q[1];"]
        if m == "movlhps":
            return [f"{V(0)}->q[1] = {V(1)}->q[0];"]
        if m == "movq":
            if isreg(0) and xr(insn, ops[0]).startswith("xmm"):
                src = f"{V(1)}->q[0]" if isreg(1) else f"R64({ft.addr(insn, ops[1])})"
                return [f"{{ uint64_t v_ = {src}; {V(0)}->q[0] = v_; {V(0)}->q[1] = 0; }}"]
            if isreg(0):  # mm
                src = f"{V(1)}->q[0]" if isreg(1) else f"R64({ft.addr(insn, ops[1])})"
                return [f"{V(0)}->q[0] = {src};"]
            return [f"W64({ft.addr(insn, ops[0])}, {V(1)}->q[0]);"]
        if m == "movd":
            if gp(0) or not isreg(0):
                return [ft.wr(insn, ops[0], f"{V(1)}->u[0]", 4)]
            src = ft.rd(insn, ops[1], 4)
            if xr(insn, ops[0]).startswith("xmm"):
                return [f"{{ uint32_t v_ = {src}; XMM *d_ = {V(0)}; d_->q[0] = d_->q[1] = 0; d_->u[0] = v_; }}"]
            return [f"c->mm[{xr(insn, ops[0])[2:]}] = {src};"]
        if m == "movmskps":
            return [ft.wr(insn, ops[0], f"(({V(1)}->u[0]>>31)|(({V(1)}->u[1]>>31)<<1)|(({V(1)}->u[2]>>31)<<2)|(({V(1)}->u[3]>>31)<<3))", 4)]
        if m == "movmskpd":
            return [ft.wr(insn, ops[0], f"(uint32_t)(({V(1)}->q[0]>>63)|(({V(1)}->q[1]>>63)<<1))", 4)]
        if m == "pmovmskb":
            return [f"{{ uint32_t r_ = 0; XMM *s_ = {V(1)}; for (int i = 0; i < 16; i++) r_ |= (uint32_t)(s_->b[i] >> 7) << i; {ft.wr(insn, ops[0], 'r_', 4)} }}"]

        # ---- scalar arithmetic
        sc = {"addss": ("f", "+"), "subss": ("f", "-"), "mulss": ("f", "*"), "divss": ("f", "/"),
              "addsd": ("d", "+"), "subsd": ("d", "-"), "mulsd": ("d", "*"), "divsd": ("d", "/")}
        if m in sc:
            t, o = sc[m]
            return [f"{V(0)}->{t}[0] = {V(0)}->{t}[0] {o} {lane_mem(1, t)};"]
        if m in ("minss", "maxss", "minsd", "maxsd"):
            t = "f" if m.endswith("ss") else "d"
            cmp = "<" if m.startswith("min") else ">"
            return [f"{{ __typeof__({V(0)}->{t}[0]) a_ = {V(0)}->{t}[0], b_ = {lane_mem(1, t)}; {V(0)}->{t}[0] = (a_ {cmp} b_) ? a_ : b_; }}"]
        if m == "sqrtss":
            return [f"{V(0)}->f[0] = sqrtf({lane_mem(1, 'f')});"]
        if m == "sqrtsd":
            return [f"{V(0)}->d[0] = sqrt({lane_mem(1, 'd')});"]
        if m == "rsqrtss":
            return [f"{V(0)}->f[0] = 1.0f / sqrtf({lane_mem(1, 'f')});"]
        if m == "rcpss":
            return [f"{V(0)}->f[0] = 1.0f / {lane_mem(1, 'f')};"]
        if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
            t = "f" if m.endswith("ss") else "d"
            return [f"{{ double x_ = {V(0)}->{t}[0], y_ = {lane_mem(1, t)}; c->of = c->sf = c->af = 0; "
                    "if (x_ != x_ || y_ != y_) { c->zf = c->pf = c->cf = 1; } else { c->pf = 0; c->zf = x_ == y_; c->cf = x_ < y_; } }"]

        # ---- compares producing masks
        for pre, sfx_list in (("cmp", ("ss", "sd", "ps", "pd")),):
            if m.startswith("cmp") and m[-2:] in sfx_list and len(m) > 5:
                pred = m[3:-2]
                sfx = m[-2:]
                expr = {"eq": "a_ == b_", "lt": "a_ < b_", "le": "a_ <= b_", "unord": "(a_ != a_ || b_ != b_)",
                        "neq": "!(a_ == b_)", "nlt": "!(a_ < b_)", "nle": "!(a_ <= b_)", "ord": "!(a_ != a_ || b_ != b_)"}.get(pred)
                if expr is None:
                    raise Unsupported("cmp pred")
                t = "f" if sfx in ("ss", "ps") else "d"
                ut = "u" if t == "f" else "q"
                ones = "0xffffffffu" if t == "f" else "~0ull"
                n = {"ss": 1, "sd": 1, "ps": 4, "pd": 2}[sfx]
                if n == 1:
                    return [f"{{ __typeof__({V(0)}->{t}[0]) a_ = {V(0)}->{t}[0], b_ = {lane_mem(1, t)}; {V(0)}->{ut}[0] = ({expr}) ? {ones} : 0; }}"]
                return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < {n}; i++) {{ __typeof__(s_.{t}[0]) a_ = d_->{t}[i], b_ = s_.{t}[i]; d_->{ut}[i] = ({expr}) ? {ones} : 0; }} }}"]

        # ---- conversions
        if m == "cvtsi2ss":
            return [f"{V(0)}->f[0] = (float)(int32_t){ft.rd(insn, ops[1], 4)};"]
        if m == "cvtsi2sd":
            return [f"{V(0)}->d[0] = (double)(int32_t){ft.rd(insn, ops[1], 4)};"]
        if m in ("cvttss2si", "cvtss2si"):
            fn = "rt_cvt_f2i_trunc" if m == "cvttss2si" else "rt_cvt_f2i_round"
            return [ft.wr(insn, ops[0], f"{fn}((double){lane_mem(1, 'f')})", 4)]
        if m in ("cvttsd2si", "cvtsd2si"):
            fn = "rt_cvt_f2i_trunc" if m == "cvttsd2si" else "rt_cvt_f2i_round"
            return [ft.wr(insn, ops[0], f"{fn}({lane_mem(1, 'd')})", 4)]
        if m == "cvtss2sd":
            return [f"{V(0)}->d[0] = (double){lane_mem(1, 'f')};"]
        if m == "cvtsd2ss":
            return [f"{V(0)}->f[0] = (float){lane_mem(1, 'd')};"]
        if m == "cvtps2pd":
            src = f"{V(1)}->q[0]" if isreg(1) else f"R64({ft.addr(insn, ops[1])})"
            return [f"{{ uint64_t q_ = {src}; float f_[2]; memcpy(f_, &q_, 8); {V(0)}->d[0] = f_[0]; {V(0)}->d[1] = f_[1]; }}"]
        if m == "cvtpd2ps":
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; d_->f[0] = (float)s_.d[0]; d_->f[1] = (float)s_.d[1]; d_->q[1] = 0; }}"]
        if m == "cvtdq2pd":
            src = f"{V(1)}->q[0]" if isreg(1) else f"R64({ft.addr(insn, ops[1])})"
            return [f"{{ uint64_t q_ = {src}; int32_t i_[2]; memcpy(i_, &q_, 8); {V(0)}->d[0] = i_[0]; {V(0)}->d[1] = i_[1]; }}"]
        if m == "cvtdq2ps":
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < 4; i++) d_->f[i] = (float)s_.i[i]; }}"]
        if m == "cvttps2dq":
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < 4; i++) d_->u[i] = rt_cvt_f2i_trunc(s_.f[i]); }}"]

        # ---- packed float / double
        pk = {"addps": ("f", 4, "+"), "subps": ("f", 4, "-"), "mulps": ("f", 4, "*"), "divps": ("f", 4, "/"),
              "addpd": ("d", 2, "+"), "subpd": ("d", 2, "-"), "mulpd": ("d", 2, "*"), "divpd": ("d", 2, "/")}
        if m in pk:
            t, n, o = pk[m]
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < {n}; i++) d_->{t}[i] = d_->{t}[i] {o} s_.{t}[i]; }}"]
        if m in ("minps", "maxps", "minpd", "maxpd"):
            t, n = ("f", 4) if m.endswith("ps") else ("d", 2)
            cmp = "<" if m.startswith("min") else ">"
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < {n}; i++) d_->{t}[i] = (d_->{t}[i] {cmp} s_.{t}[i]) ? d_->{t}[i] : s_.{t}[i]; }}"]
        if m in ("sqrtps",):
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < 4; i++) d_->f[i] = sqrtf(s_.f[i]); }}"]
        bit = {"andps": "&", "andpd": "&", "pand": "&", "orps": "|", "orpd": "|", "por": "|",
               "xorps": "^", "xorpd": "^", "pxor": "^"}
        if m in bit:
            if isreg(1) and ops[0].reg == ops[1].reg and bit[m] == "^":
                return [f"memset({V(0)}, 0, 16);" if xr(insn, ops[0]).startswith("xmm") else f"c->mm[{xr(insn, ops[0])[2:]}] = 0;"]
            n = 2 if xr(insn, ops[0]).startswith("xmm") else 1
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, {8*n}); XMM *d_ = {V(0)}; for (int i = 0; i < {n}; i++) d_->q[i] {bit[m]}= s_.q[i]; }}"]
        if m in ("andnps", "andnpd", "pandn"):
            n = 2 if xr(insn, ops[0]).startswith("xmm") else 1
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, {8*n}); XMM *d_ = {V(0)}; for (int i = 0; i < {n}; i++) d_->q[i] = ~d_->q[i] & s_.q[i]; }}"]
        if m in ("unpcklps", "unpckhps"):
            h = 2 if m == "unpckhps" else 0
            return [f"{{ XMM s_, d0_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; d0_ = *d_; d_->u[0] = d0_.u[{h}]; d_->u[1] = s_.u[{h}]; d_->u[2] = d0_.u[{h+1}]; d_->u[3] = s_.u[{h+1}]; }}"]
        if m in ("unpcklpd", "unpckhpd"):
            h = 1 if m == "unpckhpd" else 0
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; d_->q[0] = d_->q[{h}]; d_->q[1] = s_.q[{h}]; }}"]
        if m == "shufps":
            imm = ops[2].imm
            sel = [imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3]
            return [f"{{ XMM s_, d0_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; d0_ = *d_; d_->u[0] = d0_.u[{sel[0]}]; d_->u[1] = d0_.u[{sel[1]}]; d_->u[2] = s_.u[{sel[2]}]; d_->u[3] = s_.u[{sel[3]}]; }}"]
        if m == "shufpd":
            imm = ops[2].imm
            return [f"{{ XMM s_, d0_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; d0_ = *d_; d_->q[0] = d0_.q[{imm & 1}]; d_->q[1] = s_.q[{(imm >> 1) & 1}]; }}"]
        if m == "pshufd":
            imm = ops[2].imm
            sel = [imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 3]
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; " + " ".join(f"d_->u[{i}] = s_.u[{sel[i]}];" for i in range(4)) + " }"]
        if m == "pextrw":
            return [ft.wr(insn, ops[0], f"(uint32_t){V(1)}->w[{ops[2].imm & 7}]", 4)]
        if m == "pinsrw":
            src = ft.rd(insn, ops[1], 2) if not gp(1) else f"(uint16_t){ft.rd(insn, ops[1], 4)}"
            return [f"{V(0)}->w[{ops[2].imm & 7}] = {src};"]
        if m in ("psrlq", "psllq", "psrld", "pslld", "psrad", "psrlw", "psllw", "psraw", "psrldq", "pslldq"):
            is_x = xr(insn, ops[0]).startswith("xmm")
            if ops[1].type == X.X86_OP_IMM:
                cnt = str(ops[1].imm)
            else:
                cnt = f"(unsigned)({V(1)}->q[0] > 64 ? 64 : {V(1)}->q[0])"
            n = 2 if is_x else 1
            if m == "psrldq" or m == "pslldq":
                sh = ops[1].imm
                d = ">>" if m == "psrldq" else "<<"
                return [f"{{ unsigned __int128 v_; memcpy(&v_, {V(0)}, 16); v_ = ({sh} > 15) ? 0 : (v_ {d} ({sh}*8)); memcpy({V(0)}, &v_, 16); }}"]
            lane = {"q": ("q", 64, n), "d": ("u", 32, 2 * n), "w": ("w", 16, 4 * n)}[m[-1]]
            t, bits, cntl = lane
            if m.startswith("psra"):
                st = {"u": "int32_t", "w": "int16_t"}[t]
                return [f"{{ unsigned k_ = {cnt}; if (k_ > {bits-1}) k_ = {bits-1}; XMM *d_ = {V(0)}; for (int i = 0; i < {cntl}; i++) d_->{t}[i] = ({st})d_->{t}[i] >> k_; }}"]
            d = ">>" if m.startswith("psrl") else "<<"
            return [f"{{ unsigned k_ = {cnt}; XMM *d_ = {V(0)}; for (int i = 0; i < {cntl}; i++) d_->{t}[i] = (k_ >= {bits}) ? 0 : (d_->{t}[i] {d} k_); }}"]
        pint = {"paddd": ("u", "+", 4), "psubd": ("u", "-", 4), "paddq": ("q", "+", 2), "psubq": ("q", "-", 2),
                "paddw": ("w", "+", 8), "psubw": ("w", "-", 8)}
        if m in pint:
            t, o, n = pint[m]
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < {n}; i++) d_->{t}[i] = d_->{t}[i] {o} s_.{t}[i]; }}"]
        if m == "pcmpeqd":
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < 4; i++) d_->u[i] = (d_->u[i] == s_.u[i]) ? 0xffffffffu : 0; }}"]
        if m == "pmaxsw":
            return [f"{{ XMM s_; memcpy(&s_, {V(1)}, 16); XMM *d_ = {V(0)}; for (int i = 0; i < 8; i++) {{ int16_t a_ = (int16_t)d_->w[i], b_ = (int16_t)s_.w[i]; d_->w[i] = (uint16_t)(a_ > b_ ? a_ : b_); }} }}"]
        if m == "stmxcsr":
            return [f"W32({ft.addr(insn, ops[0])}, c->mxcsr);"]
        if m == "ldmxcsr":
            return [f"c->mxcsr = R32({ft.addr(insn, ops[0])});"]
        if m == "emms":
            return []
        raise Unsupported("sse rule")
