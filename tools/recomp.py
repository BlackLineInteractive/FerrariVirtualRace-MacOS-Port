"""Static recompiler: x86-32 PE (no relocations) -> C source over the CPU struct in runtime/cpu.h.

Usage: recomp.py <exe> <ghidra funcs.jsonl> <outdir>

Every guest function becomes `void f_XXXXXXXX(CPU *c)`. Intra-function branches become gotos;
direct calls become C calls; indirect calls/jumps go through call_indirect(). Flags are only
computed where a later instruction can read them (per-function liveness).
"""
import sys, os, json, struct, collections
import pefile, capstone
from capstone import x86_const as X
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fpu import Unsupported

CF, ZF, SF, OF, PF, AF, DF = 1, 2, 4, 8, 16, 32, 64
ALLF = CF | ZF | SF | OF | PF | AF
FLAGNAME = {CF: "cf", ZF: "zf", SF: "sf", OF: "of", PF: "pf", AF: "af"}

COND = {  # jcc/setcc/cmovcc suffix -> (C expr, flags read)
    "o": ("c->of", OF), "no": ("!c->of", OF),
    "b": ("c->cf", CF), "ae": ("!c->cf", CF),
    "e": ("c->zf", ZF), "ne": ("!c->zf", ZF),
    "be": ("(c->cf|c->zf)", CF | ZF), "a": ("!(c->cf|c->zf)", CF | ZF),
    "s": ("c->sf", SF), "ns": ("!c->sf", SF),
    "p": ("c->pf", PF), "np": ("!c->pf", PF),
    "l": ("(c->sf!=c->of)", SF | OF), "ge": ("(c->sf==c->of)", SF | OF),
    "le": ("(c->zf|(c->sf!=c->of))", ZF | SF | OF), "g": ("(!c->zf&&c->sf==c->of)", ZF | SF | OF),
}
JCC = {"j" + k for k in COND}

UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST_ = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}


class Image:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        self.mem[: len(pe.header)] = pe.header
        self.sections = []
        for s in pe.sections:
            d = s.get_data()[: max(s.Misc_VirtualSize, 0) or len(s.get_data())]
            self.mem[s.VirtualAddress : s.VirtualAddress + len(d)] = d
            self.sections.append((self.base + s.VirtualAddress, s.Misc_VirtualSize, s.Characteristics,
                                  s.Name.rstrip(b"\0").decode()))
        self.text = next((a, a + sz) for a, sz, ch, n in self.sections if n == ".text")
        self.imports = {}  # IAT slot address -> "dll!name"
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            for i in e.imports:
                nm = i.name.decode() if i.name else f"#{i.ordinal}"
                self.imports[i.address] = f"{e.dll.decode().lower()}!{nm}"

    def read(self, va, n):
        o = va - self.base
        return bytes(self.mem[o : o + n])

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def is_code(self, va):
        return self.text[0] <= va < self.text[1]


class FuncTranslator:
    def __init__(self, rc, entry, extra_targets):
        self.rc = rc
        self.entry = entry
        self.extra = extra_targets  # computed-jump targets known for this function
        self.ins = {}  # addr -> capstone insn
        self.labels = set()
        self.out = []
        self.notes = []

    # ---------- discovery ----------
    def discover(self):
        rc = self.rc
        work = [self.entry]
        while work:
            a = work.pop()
            while a not in self.ins:
                if not rc.img.is_code(a):
                    self.notes.append(f"flow leaves .text at {a:#x}")
                    break
                if a != self.entry and a in rc.func_entries:
                    # fell through into another function: treat as tail call
                    break
                insn = rc.decode(a)
                if insn is None:
                    self.notes.append(f"undecodable at {a:#x}")
                    break
                self.ins[a] = insn
                m = insn.mnemonic.split()[-1]
                nxt = a + insn.size
                if m in JCC or m in ("jecxz", "jcxz", "loop", "loope", "loopne"):
                    t = self.imm_target(insn)
                    if t is not None and t not in rc.func_entries:
                        work.append(t)
                    a = nxt
                    continue
                if m == "jmp":
                    t = self.imm_target(insn)
                    if t is not None:
                        if t not in rc.func_entries or t == self.entry:
                            work.append(t)
                    else:
                        if a not in self.extra:
                            tbl = self.recover_table(a, insn)
                            if tbl:
                                self.extra[a] = tbl
                        for t in self.extra.get(a, []):
                            if t not in rc.func_entries:
                                work.append(t)
                    break
                if m in ("ret", "retf", "hlt", "ud2"):
                    break
                if m == "call":
                    t = self.imm_target(insn)
                    if t is not None and rc.noreturn(t):
                        break
                if m == "int3":
                    break
                a = nxt

    def recover_table(self, a, insn):
        """jmp dword ptr [idx*4 + table]: read the table using the bound from a preceding cmp."""
        rc = self.rc
        op = insn.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            return None
        table = op.mem.disp & 0xFFFFFFFF
        prev = sorted(x for x in self.ins if x < a)[-8:]
        bound = None
        bytetab = None
        for p in reversed(prev):
            pi = self.ins[p]
            pm = pi.mnemonic.split()[-1]
            if pm == "movzx" and pi.operands[1].type == X.X86_OP_MEM and pi.operands[1].size == 1 and bytetab is None:
                bytetab = pi.operands[1].mem.disp & 0xFFFFFFFF
            if pm == "cmp" and len(pi.operands) == 2 and pi.operands[1].type == X.X86_OP_IMM:
                bound = (pi.operands[1].imm & 0xFFFFFFFF) + 1
                break
        if bound is not None and bound > 4096:
            bound = None
        count = bound
        if bytetab is not None and bound is not None:
            try:
                count = max(rc.img.read(bytetab, bound)) + 1
            except Exception:
                count = None
        targets = []
        lim = count if count is not None else 256
        for k in range(lim):
            try:
                v = rc.img.u32(table + 4 * k)
            except Exception:
                break
            if not rc.img.is_code(v) or abs(v - a) > 0x100000:
                if count is None:
                    break
                continue
            targets.append(v)
        if targets:
            self.notes.append(f"recovered jump table at {a:#x}: {len(targets)} entries")
        return sorted(set(targets)) or None

    @staticmethod
    def imm_target(insn):
        if insn.operands and insn.operands[0].type == X.X86_OP_IMM:
            return insn.operands[0].imm & 0xFFFFFFFF
        return None

    # ---------- flag liveness ----------
    def flag_effects(self, insn):
        """Return (use, def) masks."""
        m = insn.mnemonic.split()[-1]
        base = m.split()[-1]
        if base in JCC:
            return COND[base[1:]][1], 0
        if base.startswith("set") and base[3:] in COND:
            return COND[base[3:]][1], 0
        if base.startswith("cmov") and base[4:] in COND:
            return COND[base[4:]][1], 0
        if base.startswith("fcmov"):
            return CF | ZF | PF, 0
        if base in ("add", "sub", "cmp", "neg", "xadd", "cmpxchg"):
            return 0, ALLF
        if base in ("adc", "sbb"):
            return CF, ALLF
        if base in ("and", "or", "xor", "test"):
            return 0, ALLF
        if base in ("inc", "dec"):
            return 0, ALLF & ~CF
        if base in ("shl", "sal", "shr", "sar"):
            ops = insn.operands
            if len(ops) == 1 or (ops[1].type == X.X86_OP_IMM and ops[1].imm & 31):
                return 0, ALLF
            return ALLF, ALLF  # count may be 0 -> flags unchanged
        if base in ("rol", "ror"):
            ops = insn.operands
            if len(ops) == 1 or (ops[1].type == X.X86_OP_IMM and ops[1].imm & 31):
                return 0, CF | OF
            return CF | OF, CF | OF
        if base in ("rcl", "rcr"):
            return CF, CF
        if base in ("shld", "shrd"):
            return ALLF, ALLF
        if base in ("imul", "mul"):
            return 0, ALLF
        if base in ("div", "idiv"):
            return 0, ALLF
        if base in ("bt", "bts", "btr", "btc"):
            return 0, CF
        if base in ("bsf", "bsr"):
            return 0, ZF
        if base in ("comiss", "ucomiss", "comisd", "ucomisd", "fcomi", "fcomip", "fucomi", "fucomip"):
            return 0, ALLF
        if base == "sahf":
            return 0, CF | ZF | SF | PF | AF
        if base in ("lahf", "pushfd", "pushf"):
            return ALLF, 0
        if base in ("popfd", "popf"):
            return 0, ALLF
        if base in ("stc", "clc", "cmc"):
            return (CF if base == "cmc" else 0), CF
        if base in ("cmpsb", "cmpsw", "cmpsd", "scasb", "scasw", "scasd") and not base.startswith("cmpsd xmm"):
            if insn.op_str and "xmm" in insn.op_str:
                return 0, 0
            return (ZF if insn.prefix[0] in (0xF2, 0xF3) else 0), ALLF
        if base in ("das", "daa", "aaa", "aas"):
            return ALLF, ALLF
        return 0, 0

    def successors(self, a):
        insn = self.ins[a]
        m = insn.mnemonic.split()[-1]
        nxt = a + insn.size
        if m in ("ret", "retf", "hlt", "ud2", "int3"):
            return []
        if m == "jmp":
            t = self.imm_target(insn)
            if t is not None:
                return [t] if t in self.ins else []
            return [t for t in self.extra.get(a, []) if t in self.ins]
        out = [nxt] if nxt in self.ins else []
        if m in JCC or m in ("jecxz", "loop", "loope", "loopne"):
            t = self.imm_target(insn)
            if t in self.ins:
                out.append(t)
        return out

    def liveness(self):
        effects = {a: self.flag_effects(i) for a, i in self.ins.items()}
        succ = {a: self.successors(a) for a in self.ins}
        # anything leaving the function (calls, returns, tail jumps): flags dead by convention
        live_in = {a: 0 for a in self.ins}
        changed = True
        order = sorted(self.ins, reverse=True)
        while changed:
            changed = False
            for a in order:
                out = 0
                for s in succ[a]:
                    out |= live_in[s]
                use, d = effects[a]
                if self.ins[a].mnemonic == "call":
                    out = 0
                li = use | (out & ~d)
                if li != live_in[a]:
                    live_in[a] = li
                    changed = True
        self.live_out = {}
        for a in self.ins:
            o = 0
            for s in succ[a]:
                o |= live_in[s]
            self.live_out[a] = o

    # ---------- operands ----------
    def reg_read(self, name):
        if name in REG32:
            return f"c->{name}"
        if name in REG16:
            return f"((uint16_t)c->{REG16[name]})"
        if name in REG8L:
            return f"((uint8_t)c->{REG8L[name]})"
        if name in REG8H:
            return f"((uint8_t)(c->{REG8H[name]}>>8))"
        raise Unsupported(f"reg {name}")

    def reg_write(self, name, v):
        if name in REG32:
            return f"c->{name} = (uint32_t)({v});"
        if name in REG16:
            r = REG16[name]
            return f"c->{r} = (c->{r} & 0xffff0000u) | (uint16_t)({v});"
        if name in REG8L:
            r = REG8L[name]
            return f"c->{r} = (c->{r} & 0xffffff00u) | (uint8_t)({v});"
        if name in REG8H:
            r = REG8H[name]
            return f"c->{r} = (c->{r} & 0xffff00ffu) | ((uint32_t)(uint8_t)({v}) << 8);"
        raise Unsupported(f"reg {name}")

    def addr(self, insn, op):
        mem = op.mem
        parts = []
        if mem.base:
            parts.append(self.reg_read(insn.reg_name(mem.base)))
        if mem.index:
            idx = self.reg_read(insn.reg_name(mem.index))
            parts.append(f"{idx}*{mem.scale}" if mem.scale != 1 else idx)
        if mem.disp or not parts:
            parts.append(f"0x{mem.disp & 0xFFFFFFFF:x}u")
        if mem.segment and insn.reg_name(mem.segment) == "fs":
            parts.append("c->fs_base")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def rd(self, insn, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            n = insn.reg_name(op.reg)
            if n.startswith("xmm"):
                raise Unsupported("xmm as gp")
            return self.reg_read(n)
        if op.type == X.X86_OP_IMM:
            return f"(({UT[size]})0x{op.imm & ((1 << (8 * size)) - 1):x}u)"
        if op.type == X.X86_OP_MEM:
            fn = {1: "R8", 2: "R16", 4: "R32", 8: "R64"}[size]
            return f"{fn}({self.addr(insn, op)})"
        raise Unsupported("operand")

    def wr(self, insn, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(insn.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            fn = {1: "W8", 2: "W16", 4: "W32", 8: "W64"}[size]
            return f"{fn}({self.addr(insn, op)}, ({UT[size]})({v}));"
        raise Unsupported("write operand")

    # ---------- flag helpers ----------
    def flags_logic(self, live, res, size):
        s = []
        if live & ZF: s.append(f"c->zf = ({res}) == 0;")
        if live & SF: s.append(f"c->sf = (({res}) >> {8*size-1}) & 1;")
        if live & PF: s.append(f"c->pf = PARITY({res});")
        if live & CF: s.append("c->cf = 0;")
        if live & OF: s.append("c->of = 0;")
        if live & AF: s.append("c->af = 0;")
        return s

    def flags_add(self, live, a, b, res, size, carry_in=None):
        s = self.flags_logic(live & (ZF | SF | PF), res, size)
        if live & CF:
            if carry_in:
                s.append(f"c->cf = ({res} < {a}) || ({carry_in} && {res} == {a});")
            else:
                s.append(f"c->cf = {res} < {a};")
        if live & OF: s.append(f"c->of = ((({a}^{res})&({b}^{res})) >> {8*size-1}) & 1;")
        if live & AF: s.append(f"c->af = (({a}^{b}^{res}) >> 4) & 1;")
        return s

    def flags_sub(self, live, a, b, res, size, borrow_in=None):
        s = self.flags_logic(live & (ZF | SF | PF), res, size)
        if live & CF:
            if borrow_in:
                s.append(f"c->cf = ({a} < {b}) || ({borrow_in} && {a} == {b});")
            else:
                s.append(f"c->cf = {a} < {b};")
        if live & OF: s.append(f"c->of = ((({a}^{b})&({a}^{res})) >> {8*size-1}) & 1;")
        if live & AF: s.append(f"c->af = (({a}^{b}^{res}) >> 4) & 1;")
        return s

    # ---------- translation ----------
    def translate(self):
        self.discover()
        self.liveness()
        for a, insn in self.ins.items():
            for t in self.successors(a):
                if t != a + insn.size or insn.mnemonic.split()[-1] == "jmp":
                    self.labels.add(t)
        name = self.rc.fname(self.entry)
        o = [f"void {name}(CPU *restrict c) {{"]
        if min(self.ins) != self.entry:
            self.labels.add(self.entry)
            o.append(f"  goto L_{self.entry:x};")
        prev_end = None
        for a in sorted(self.ins):
            insn = self.ins[a]
            if a in self.labels:
                o.append(f"L_{a:x}:;")
            if prev_end is not None and prev_end != a and prev_end in self.ins:
                pass
            try:
                body = self.insn(insn)
            except Unsupported as e:
                body = [f'rt_unimpl(c, 0x{a:x}u, "{insn.mnemonic} {insn.op_str}: {e}");']
                self.rc.unsupported[insn.mnemonic] += 1
            o.append(f"  /* {a:08x}: {insn.mnemonic} {insn.op_str} */")
            o.extend("  " + l for l in body)
            prev_end = a + insn.size
            m = insn.mnemonic.split()[-1]
            # code falls off the end of a decoded run -> continue into next address
            if m not in ("ret", "jmp", "int3", "hlt", "ud2") and prev_end not in self.ins:
                tgt = prev_end
                if tgt in self.rc.func_entries:
                    o.append(f"  {self.rc.fname(tgt)}(c); return;")
                elif not (m == "call" and self.imm_target(insn) is not None and self.rc.noreturn(self.imm_target(insn))):
                    o.append(f'  rt_unimpl(c, 0x{tgt:x}u, "fell off decoded code");')
        o.append("}")
        return o

    def jump_to(self, t):
        if t in self.ins:
            return f"goto L_{t:x};"
        if t in self.rc.func_entries:
            return f"{{ {self.rc.fname(t)}(c); return; }}"
        return f'rt_unimpl(c, 0x{t:x}u, "jump to unknown");'

    def call_target(self, t, ret):
        rc = self.rc
        if t in rc.func_entries:
            return [f"PUSH(c, 0x{ret:x}u); {rc.fname(t)}(c);"]
        rc.missing_calls.add(t)
        return [f"PUSH(c, 0x{ret:x}u); call_indirect(c, 0x{t:x}u);"]

    def insn(self, insn):
        m = insn.mnemonic.split()[-1]
        ops = insn.operands
        a = insn.address
        nxt = a + insn.size
        live = self.live_out.get(a, 0)
        if a in self.rc.hooks:
            return [f"{self.rc.hooks[a]}(c);"]
        prefix_rep = insn.prefix[0] in (0xF3, 0xF2)

        if m in ("les", "lds", "lcall", "ljmp", "into", "in", "out", "iretd"):
            return [f'rt_unimpl(c, 0x{a:x}u, "{insn.mnemonic} {insn.op_str}");']
        if m in ("nop", "int3", "fnop", "pause", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "lfence", "sfence", "mfence", "fwait", "wait"):
            if m == "int3":
                return [f'rt_unimpl(c, 0x{a:x}u, "int3");']
            return []

        # ---------------- control flow ----------------
        if m in JCC:
            cond, _ = COND[m[1:]]
            return [f"if ({cond}) {self.jump_to(self.imm_target(insn))}"]
        if m == "jecxz":
            return [f"if (c->ecx == 0) {self.jump_to(self.imm_target(insn))}"]
        if m == "loop":
            return [f"if (--c->ecx != 0) {self.jump_to(self.imm_target(insn))}"]
        if m == "jmp":
            t = self.imm_target(insn)
            if t is not None:
                return [self.jump_to(t)]
            tgt = self.rd(insn, ops[0], 4)
            tg = self.extra.get(a)
            if tg:
                cases = " ".join(f"case 0x{x:x}u: goto L_{x:x};" for x in sorted(set(tg)) if x in self.ins)
                return [f"{{ uint32_t t = {tgt}; switch (t) {{ {cases} default: call_indirect(c, t); return; }} }}"]
            return [f"call_indirect(c, {tgt}); return;"]
        if m == "call":
            t = self.imm_target(insn)
            if t is not None:
                if t == nxt:  # call $+5 ; pop reg  (get PC)
                    return [f"PUSH(c, 0x{nxt:x}u);"]
                return self.call_target(t, nxt)
            return [f"{{ uint32_t t = {self.rd(insn, ops[0], 4)}; PUSH(c, 0x{nxt:x}u); call_indirect(c, t); }}"]
        if m == "ret":
            n = ops[0].imm if ops else 0
            return [f"c->esp += {4 + n}; return;"]

        # ---------------- data movement ----------------
        if m == "mov":
            if ops[0].type == X.X86_OP_REG and insn.reg_name(ops[0].reg) in ("fs", "gs", "es", "ds", "ss", "cs"):
                return []
            if ops[1].type == X.X86_OP_REG and insn.reg_name(ops[1].reg) in ("fs", "gs", "es", "ds", "ss", "cs"):
                return [self.wr(insn, ops[0], "0x23")]
            return [self.wr(insn, ops[0], self.rd(insn, ops[1]))]
        if m in ("movzx", "movsx"):
            ts = ST_ if m == "movsx" else UT
            v = f"({UT[ops[0].size]})({ts[ops[1].size]}){self.rd(insn, ops[1])}"
            return [self.wr(insn, ops[0], v)]
        if m == "lea":
            return [self.wr(insn, ops[0], self.addr(insn, ops[1]).replace(" + c->fs_base", ""))]
        if m == "push":
            if ops[0].type == X.X86_OP_REG and insn.reg_name(ops[0].reg) in ("fs", "es", "ds", "ss", "cs", "gs"):
                return ["PUSH(c, 0x23u);"]
            if ops[0].size == 2:
                return [f"c->esp -= 2; W16(c->esp, {self.rd(insn, ops[0])});"]
            if ops[0].type == X.X86_OP_IMM:
                return [f"PUSH(c, 0x{ops[0].imm & 0xFFFFFFFF:x}u);"]
            return [f"{{ uint32_t v = {self.rd(insn, ops[0], 4)}; PUSH(c, v); }}"]
        if m == "pop":
            if ops[0].type == X.X86_OP_REG and insn.reg_name(ops[0].reg) in ("fs", "es", "ds", "ss", "cs", "gs"):
                return ["c->esp += 4;"]
            if ops[0].type == X.X86_OP_MEM:
                return [f"{{ uint32_t v = POP(c); {self.wr(insn, ops[0], 'v')} }}"]
            return [f"{{ uint32_t v = POP(c); {self.wr(insn, ops[0], 'v')} }}"]
        if m == "pushal":
            return ["{ uint32_t s = c->esp; PUSH(c,c->eax); PUSH(c,c->ecx); PUSH(c,c->edx); PUSH(c,c->ebx); PUSH(c,s); PUSH(c,c->ebp); PUSH(c,c->esi); PUSH(c,c->edi); }"]
        if m == "popal":
            return ["c->edi=POP(c); c->esi=POP(c); c->ebp=POP(c); c->esp+=4; c->ebx=POP(c); c->edx=POP(c); c->ecx=POP(c); c->eax=POP(c);"]
        if m in ("pushfd", "pushf"):
            return ["PUSH(c, 0x202u | c->cf | (c->pf<<2) | (c->af<<4) | (c->zf<<6) | (c->sf<<7) | (c->df<<10) | (c->of<<11));"]
        if m in ("popfd", "popf"):
            return ["{ uint32_t f = POP(c); c->cf=f&1; c->pf=(f>>2)&1; c->af=(f>>4)&1; c->zf=(f>>6)&1; c->sf=(f>>7)&1; c->df=(f>>10)&1; c->of=(f>>11)&1; }"]
        if m == "xchg":
            if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return []
            sz = ops[0].size
            return [f"{{ {UT[sz]} t0 = {self.rd(insn, ops[0])}, t1 = {self.rd(insn, ops[1])}; "
                    f"{self.wr(insn, ops[0], 't1')} {self.wr(insn, ops[1], 't0')} }}"]
        if m == "xlatb":
            return [self.reg_write("al", "R8(c->ebx + (uint8_t)c->eax)")]
        if m in ("rcr", "rcl") and ops[0].type in (X.X86_OP_REG, X.X86_OP_MEM):
            sz = ops[0].size
            bits = 8 * sz
            cnt = self.rd(insn, ops[1]) if len(ops) > 1 else "1"
            v = self.rd(insn, ops[0])
            s = [f"{{ uint64_t v = {v}; unsigned n = (({cnt}) & 31) % {bits + 1}; uint64_t cf = c->cf;",
                 f"  for (unsigned i = 0; i < n; i++) {{"]
            if m == "rcr":
                s.append(f"    uint64_t nc = v & 1; v = (v >> 1) | (cf << {bits - 1}); cf = nc; }}")
            else:
                s.append(f"    uint64_t nc = (v >> {bits - 1}) & 1; v = ((v << 1) | cf) & 0x{(1 << bits) - 1:x}ull; cf = nc; }}")
            s.append("  if (n) c->cf = (uint8_t)cf;")
            s.append("  " + self.wr(insn, ops[0], f"({UT[sz]})v") + " }")
            return s
        if m == "cdq":
            return ["c->edx = (uint32_t)((int32_t)c->eax >> 31);"]
        if m == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)c->eax;"]
        if m == "cbw":
            return [self.reg_write("ax", "(int16_t)(int8_t)c->eax")]
        if m == "cwd":
            return [self.reg_write("dx", "((int16_t)c->eax < 0) ? 0xffff : 0")]
        if m == "leave":
            return ["c->esp = c->ebp; c->ebp = POP(c);"]
        if m == "lahf":
            return ["c->eax = (c->eax & 0xffff00ffu) | ((uint32_t)(0x02u | c->cf | (c->pf<<2) | (c->af<<4) | (c->zf<<6) | (c->sf<<7)) << 8);"]
        if m == "sahf":
            return ["{ uint32_t f = (c->eax >> 8) & 0xff; c->cf=f&1; c->pf=(f>>2)&1; c->af=(f>>4)&1; c->zf=(f>>6)&1; c->sf=(f>>7)&1; }"]
        if m == "cld":
            return ["c->df = 0;"]
        if m == "std":
            return ["c->df = 1;"]
        if m == "clc":
            return ["c->cf = 0;"]
        if m == "stc":
            return ["c->cf = 1;"]
        if m == "cmc":
            return ["c->cf ^= 1;"]
        if m.startswith("set") and m[3:] in COND:
            return [self.wr(insn, ops[0], f"({COND[m[3:]][0]}) ? 1 : 0")]
        if m.startswith("cmov") and m[4:] in COND:
            return [f"if ({COND[m[4:]][0]}) {self.wr(insn, ops[0], self.rd(insn, ops[1]))}"]
        if m == "bswap":
            return [self.wr(insn, ops[0], f"__builtin_bswap32({self.rd(insn, ops[0])})")]

        # ---------------- integer ALU ----------------
        if m in ("add", "sub", "cmp", "adc", "sbb", "and", "or", "xor", "test"):
            sz = ops[0].size
            t = UT[sz]
            # xor r,r / sub r,r idiom
            if m in ("xor", "sub") and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                s = [self.wr(insn, ops[0], "0")]
                s += self.flags_logic(live, "0u", sz)
                if live & ZF: s.append("c->zf = 1;")
                return s
            av = self.rd(insn, ops[0])
            bv = f"({t})(" + self.rd(insn, ops[1], sz if ops[1].type != X.X86_OP_IMM else sz) + ")"
            if ops[1].type == X.X86_OP_IMM:
                bv = f"(({t})0x{ops[1].imm & ((1 << (8*sz)) - 1):x}u)"
            s = [f"{{ {t} a = {av}, b = {bv};"]
            if m in ("add", "adc"):
                if m == "adc":
                    s.append("  uint8_t ci = c->cf;")
                    s.append(f"  {t} r = ({t})(a + b + ci);")
                    s += ["  " + x for x in self.flags_add(live, "a", "b", "r", sz, "ci")]
                else:
                    s.append(f"  {t} r = ({t})(a + b);")
                    s += ["  " + x for x in self.flags_add(live, "a", "b", "r", sz)]
                s.append("  " + self.wr(insn, ops[0], "r"))
            elif m in ("sub", "cmp", "sbb"):
                if m == "sbb":
                    s.append("  uint8_t bi = c->cf;")
                    s.append(f"  {t} r = ({t})(a - b - bi);")
                    s += ["  " + x for x in self.flags_sub(live, "a", "b", "r", sz, "bi")]
                else:
                    s.append(f"  {t} r = ({t})(a - b);")
                    s += ["  " + x for x in self.flags_sub(live, "a", "b", "r", sz)]
                if m != "cmp":
                    s.append("  " + self.wr(insn, ops[0], "r"))
            else:
                opc = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
                s.append(f"  {t} r = a {opc} b;")
                s += ["  " + x for x in self.flags_logic(live, "r", sz)]
                if m != "test":
                    s.append("  " + self.wr(insn, ops[0], "r"))
            s.append("}")
            return s
        if m in ("inc", "dec", "neg", "not"):
            sz = ops[0].size
            t = UT[sz]
            s = [f"{{ {t} a = {self.rd(insn, ops[0])};"]
            if m == "inc":
                s.append(f"  {t} r = ({t})(a + 1);")
                s += ["  " + x for x in self.flags_add(live & ~CF, "a", f"({t})1", "r", sz)]
            elif m == "dec":
                s.append(f"  {t} r = ({t})(a - 1);")
                s += ["  " + x for x in self.flags_sub(live & ~CF, "a", f"({t})1", "r", sz)]
            elif m == "neg":
                s.append(f"  {t} r = ({t})(0 - a);")
                s += ["  " + x for x in self.flags_sub(live & ~CF, f"({t})0", "a", "r", sz)]
                if live & CF: s.append("  c->cf = a != 0;")
            else:
                s.append(f"  {t} r = ({t})~a;")
            s.append("  " + self.wr(insn, ops[0], "r"))
            s.append("}")
            return s
        if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            sz = ops[0].size
            t = UT[sz]
            bits = 8 * sz
            cnt = "1" if len(ops) == 1 else (self.rd(insn, ops[1]) if ops[1].type == X.X86_OP_REG else str(ops[1].imm))
            s = [f"{{ unsigned n = ({cnt}) & 31; if (n) {{ {t} a = {self.rd(insn, ops[0])}; {t} r;"]
            if m in ("shl", "sal"):
                s.append(f"  r = ({t})((uint64_t)a << n);")
                if live & CF: s.append(f"  c->cf = (n <= {bits}) ? (((uint64_t)a >> ({bits} - n)) & 1) : 0;")
                if live & OF: s.append(f"  c->of = ((r >> {bits-1}) & 1) ^ c->cf;")
            elif m == "shr":
                s.append(f"  r = ({t})((uint64_t)a >> n);")
                if live & CF: s.append("  c->cf = ((uint64_t)a >> (n - 1)) & 1;")
                if live & OF: s.append(f"  c->of = (a >> {bits-1}) & 1;")
            elif m == "sar":
                s.append(f"  r = ({t})((int64_t)({ST_[sz]})a >> n);")
                if live & CF: s.append(f"  c->cf = ((int64_t)({ST_[sz]})a >> (n - 1)) & 1;")
                if live & OF: s.append("  c->of = 0;")
            elif m == "rol":
                s.append(f"  unsigned k = n % {bits}; r = ({t})((a << k) | (a >> (({bits} - k) % {bits})));")
                if live & CF: s.append("  c->cf = r & 1;")
                if live & OF: s.append(f"  c->of = ((r >> {bits-1}) & 1) ^ (r & 1);")
            elif m == "ror":
                s.append(f"  unsigned k = n % {bits}; r = ({t})((a >> k) | (a << (({bits} - k) % {bits})));")
                if live & CF: s.append(f"  c->cf = (r >> {bits-1}) & 1;")
                if live & OF: s.append(f"  c->of = ((r >> {bits-1}) ^ (r >> {bits-2})) & 1;")
            if m in ("shl", "sal", "shr", "sar"):
                s += ["  " + x for x in self.flags_logic(live & (ZF | SF | PF), "r", sz)]
            s.append("  " + self.wr(insn, ops[0], "r"))
            s.append("} }")
            return s
        if m in ("shld", "shrd"):
            cnt = self.rd(insn, ops[2]) if ops[2].type == X.X86_OP_REG else str(ops[2].imm)
            d, src = self.rd(insn, ops[0]), self.rd(insn, ops[1])
            if m == "shld":
                expr = "(uint32_t)((((uint64_t)a << 32) | b) << n >> 32)"
                cf = "c->cf = (a >> (32 - n)) & 1;"
            else:
                expr = "(uint32_t)((((uint64_t)b << 32) | a) >> n)"
                cf = "c->cf = (a >> (n - 1)) & 1;"
            s = [f"{{ unsigned n = ({cnt}) & 31; if (n) {{ uint32_t a = {d}, b = {src}; uint32_t r = {expr};"]
            if live & CF: s.append("  " + cf)
            if live & OF: s.append("  c->of = ((r ^ a) >> 31) & 1;")
            s += ["  " + x for x in self.flags_logic(live & (ZF | SF | PF), "r", 4)]
            s.append("  " + self.wr(insn, ops[0], "r") + " } }")
            return s
        if m == "imul":
            if len(ops) == 1:
                sz = ops[0].size
                if sz == 4:
                    s = [f"{{ int64_t r = (int64_t)(int32_t)c->eax * (int32_t){self.rd(insn, ops[0])}; c->eax = (uint32_t)r; c->edx = (uint32_t)(r >> 32);"]
                    if live & (CF | OF): s.append("  c->cf = c->of = r != (int64_t)(int32_t)r;")
                    return s + ["}"]
                if sz == 2:
                    s = [f"{{ int32_t r = (int32_t)(int16_t)c->eax * (int16_t){self.rd(insn, ops[0])}; {self.reg_write('ax', '(uint16_t)r')} {self.reg_write('dx', '(uint16_t)(r >> 16)')}"]
                    if live & (CF | OF): s.append("  c->cf = c->of = r != (int32_t)(int16_t)r;")
                    return s + ["}"]
                s = [f"{{ int16_t r = (int16_t)(int8_t)c->eax * (int8_t){self.rd(insn, ops[0])}; {self.reg_write('ax', '(uint16_t)r')}"]
                if live & (CF | OF): s.append("  c->cf = c->of = r != (int16_t)(int8_t)r;")
                return s + ["}"]
            if len(ops) == 2:
                d, b = self.rd(insn, ops[0]), self.rd(insn, ops[1])
            else:
                d, b = self.rd(insn, ops[1]), self.rd(insn, ops[2])
            sz = ops[0].size
            s = [f"{{ int64_t r = (int64_t)({ST_[sz]}){d} * (int64_t)({ST_[sz]}){b};"]
            if live & (CF | OF): s.append(f"  c->cf = c->of = r != (int64_t)({ST_[sz]})r;")
            if live & ZF: s.append(f"  c->zf = ({UT[sz]})r == 0;")
            if live & SF: s.append(f"  c->sf = (({UT[sz]})r >> {8*sz-1}) & 1;")
            s.append("  " + self.wr(insn, ops[0], f"({UT[sz]})r") + " }")
            return s
        if m == "mul":
            sz = ops[0].size
            if sz == 4:
                s = [f"{{ uint64_t r = (uint64_t)c->eax * {self.rd(insn, ops[0])}; c->eax = (uint32_t)r; c->edx = (uint32_t)(r >> 32);"]
                if live & (CF | OF): s.append("  c->cf = c->of = c->edx != 0;")
                return s + ["}"]
            raise Unsupported("mul size")
        if m in ("div", "idiv"):
            sz = ops[0].size
            if sz != 4:
                raise Unsupported("div size")
            if m == "div":
                return [f"{{ uint64_t n = ((uint64_t)c->edx << 32) | c->eax; uint32_t d = {self.rd(insn, ops[0])}; "
                        f"c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); }}"]
            return [f"{{ int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); int32_t d = (int32_t){self.rd(insn, ops[0])}; "
                    f"c->eax = (uint32_t)(int32_t)(n / d); c->edx = (uint32_t)(int32_t)(n % d); }}"]
        if m in ("bt", "bts", "btr", "btc"):
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
                # bit string: the bit offset may reach outside the addressed dword
                a = self.addr(insn, ops[0])
                s = [f"{{ int32_t bo = (int32_t){self.rd(insn, ops[1])}; uint32_t a = {a} + (uint32_t)(bo >> 3); unsigned b = bo & 7; uint8_t v = R8(a); c->cf = (v >> b) & 1;"]
                if m == "bts": s.append("  W8(a, v | (1u << b));")
                if m == "btr": s.append("  W8(a, v & ~(1u << b));")
                if m == "btc": s.append("  W8(a, v ^ (1u << b));")
                return s + ["}"]
            bit = self.rd(insn, ops[1])
            v = self.rd(insn, ops[0])
            s = [f"{{ uint32_t v = {v}; unsigned b = ({bit}) & {8*ops[0].size-1}; c->cf = (v >> b) & 1;"]
            if m == "bts": s.append("  " + self.wr(insn, ops[0], "v | (1u << b)"))
            if m == "btr": s.append("  " + self.wr(insn, ops[0], "v & ~(1u << b)"))
            if m == "btc": s.append("  " + self.wr(insn, ops[0], "v ^ (1u << b)"))
            return s + ["}"]
        if m in ("bsf", "bsr"):
            v = self.rd(insn, ops[1])
            fn = "__builtin_ctz(v)" if m == "bsf" else "(31 - __builtin_clz(v))"
            return [f"{{ uint32_t v = {v}; c->zf = v == 0; if (v) {self.wr(insn, ops[0], fn)} }}"]
        if m in ("xadd",):
            sz = ops[0].size
            t = UT[sz]
            s = [f"{{ {t} a = {self.rd(insn, ops[0])}, b = {self.rd(insn, ops[1])}; {t} r = ({t})(a + b);"]
            s += ["  " + x for x in self.flags_add(live, "a", "b", "r", sz)]
            s.append("  " + self.wr(insn, ops[1], "a") + " " + self.wr(insn, ops[0], "r") + " }")
            return s
        if m == "cmpxchg":
            sz = ops[0].size
            t = UT[sz]
            acc = {4: "eax", 2: "ax", 1: "al"}[sz]
            s = [f"{{ {t} a = {self.reg_read(acc)}, b = {self.rd(insn, ops[0])}; {t} r = ({t})(a - b);"]
            s += ["  " + x for x in self.flags_sub(live | ZF, "a", "b", "r", sz)]
            s.append(f"  if (a == b) {{ {self.wr(insn, ops[0], self.rd(insn, ops[1]))} }} else {{ {self.reg_write(acc, 'b')} }} }}")
            return s

        # ---------------- string ops ----------------
        if m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsd", "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd") and not (ops and any(o.type == X.X86_OP_REG and insn.reg_name(o.reg).startswith("xmm") for o in ops)):
            return self.string_op(insn, m, prefix_rep, live)

        # ---------------- misc ----------------
        if m == "cpuid":
            return ["rt_cpuid(c);"]
        if m == "rdtsc":
            return ["{ uint64_t t = rt_rdtsc(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }"]
        if m in ("hlt", "ud2"):
            return [f'rt_unimpl(c, 0x{a:x}u, "{m}");']

        # ---------------- x87 / SSE / MMX ----------------
        r = self.rc.fpu.translate(self, insn)
        if r is not None:
            return r
        raise Unsupported("no rule")

    def string_op(self, insn, m, rep, live):
        size = {"b": 1, "w": 2, "d": 4}[m[-1]]
        t = UT[size]
        R = {1: "R8", 2: "R16", 4: "R32"}[size]
        W = {1: "W8", 2: "W16", 4: "W32"}[size]
        acc = {1: "(uint8_t)c->eax", 2: "(uint16_t)c->eax", 4: "c->eax"}[size]
        step = f"(c->df ? -{size} : {size})"
        if m.startswith("movs"):
            body = f"{W}(c->edi, {R}(c->esi)); c->esi += {step}; c->edi += {step};"
            if rep:
                return [f"if (!c->df) {{ memmove(H(c->edi), H(c->esi), (size_t)c->ecx * {size}); c->esi += c->ecx*{size}; c->edi += c->ecx*{size}; c->ecx = 0; }}",
                        f"else while (c->ecx) {{ {body} c->ecx--; }}"]
            return [body]
        if m.startswith("stos"):
            body = f"{W}(c->edi, {acc}); c->edi += {step};"
            if rep:
                if size == 1:
                    return [f"if (!c->df) {{ memset(H(c->edi), (uint8_t)c->eax, c->ecx); c->edi += c->ecx; c->ecx = 0; }}",
                            f"else while (c->ecx) {{ {body} c->ecx--; }}"]
                return [f"while (c->ecx) {{ {body} c->ecx--; }}"]
            return [body]
        if m.startswith("lods"):
            body = self.reg_write({1: "al", 2: "ax", 4: "eax"}[size], f"{R}(c->esi)") + f" c->esi += {step};"
            if rep:
                return [f"while (c->ecx) {{ {body} c->ecx--; }}"]
            return [body]
        if m.startswith("scas") or m.startswith("cmps"):
            if m.startswith("scas"):
                cmpe = f"{t} a = {acc}, b = {R}(c->edi); c->edi += {step};"
            else:
                cmpe = f"{t} a = {R}(c->esi), b = {R}(c->edi); c->esi += {step}; c->edi += {step};"
            fl = " ".join(self.flags_sub(ALLF, "a", "b", "r", size))
            one = f"{{ {cmpe} {t} r = ({t})(a - b); {fl} }}"
            if not rep:
                return [one]
            stop = "!c->zf" if insn.prefix[0] == 0xF3 else "c->zf"
            return [f"while (c->ecx) {{ {one} c->ecx--; if ({stop}) break; }}"]
        raise Unsupported("string op")


class Recompiler:
    def __init__(self, exe, funcs_path, outdir):
        self.img = Image(exe)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.outdir = outdir
        self.func_entries = set()
        self.cjumps = {}
        for line in open(funcs_path):
            j = json.loads(line)
            if j["type"] == "func":
                if self.img.is_code(j["entry"]):
                    self.func_entries.add(j["entry"])
            elif j["type"] == "cjump":
                if j["targets"]:
                    self.cjumps[j["at"]] = j["targets"]
        self.cjump_targets = {t for ts in self.cjumps.values() for t in ts}
        ep = self.img.base + self.img.pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.func_entries.add(ep)
        self.entry_point = ep
        self._dec = {}
        self._noreturn = set()
        self.add_vtable_targets()
        self.add_code_immediates()
        extra = os.path.join(os.path.dirname(os.path.abspath(__file__)), "extra_entries.txt")
        if os.path.exists(extra):
            for line in open(extra):
                line = line.split("#")[0].strip()
                if line:
                    self.func_entries.add(int(line, 16))
        # host hooks: replace one guest instruction with a call into the runtime ("addr name" per line)
        self.hooks = {}
        hk = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hooks.txt")
        if os.path.exists(hk):
            for line in open(hk):
                line = line.split("#")[0].split()
                if len(line) == 2:
                    self.hooks[int(line[0], 16)] = line[1]
        self.unsupported = collections.Counter()
        self.missing_calls = set()
        import fpu
        self.fpu = fpu.FpuSse()

    def add_code_immediates(self):
        """Immediates in code that point at plausible function starts (function pointers passed by value)."""
        lo, hi = self.img.text
        total = 0
        pending = set(self.func_entries)
        seen = set()
        while pending:
            new = set()
            for a in pending:
                seen.add(a)
                ft = FuncTranslator(self, a, self.cjumps)
                ft.discover()
                for insn in ft.ins.values():
                    loose = insn.mnemonic.split()[-1] in ("push", "mov", "lea")
                    for op in insn.operands:
                        if op.type != X.X86_OP_IMM:
                            continue
                        v = op.imm & 0xFFFFFFFF
                        if lo <= v < hi and v not in self.func_entries and v not in new:
                            prev = self.img.read(v - 1, 1)
                            ok = prev in (b"\xcc", b"\xc3", b"\x90") or self.img.read(v - 3, 1) == b"\xc2"
                            # right after a switch jump table (previous dword is a code address)
                            if not ok and loose and self.img.is_code(self.img.u32(v - 4)):
                                ok = True
                            # compiler-generated stubs packed back to back after a jmp rel32 (atexit destructors)
                            if not ok and loose and self.img.read(v - 5, 1) == b"\xe9":
                                ok = True
                            # MSVC pads functions to 16 bytes
                            if not ok and loose and v % 16 == 0 and v not in self.cjump_targets:
                                ok = True
                            if ok and v not in self.cjump_targets and self.decode(v) is not None and self.decodes_cleanly(v):
                                new.add(v)
            self.func_entries |= new
            total += len(new)
            pending = new - seen
        print("code-immediate function pointers added:", total)

    def add_vtable_targets(self):
        """Any aligned dword in data sections pointing at a plausible function start becomes an entry."""
        added = 0
        for va, sz, ch, name in self.img.sections:
            if name not in (".rdata", ".data"):
                continue
            for off in range(0, sz - 3, 4):
                v = self.img.u32(va + off)
                if self.img.is_code(v) and v not in self.func_entries:
                    insn = self.decode(v)
                    if insn is not None and v not in self.cjump_targets and self.decodes_cleanly(v):
                        self.func_entries.add(v)
                        added += 1
        print("vtable/data code pointers added as entries:", added)

    def decodes_cleanly(self, a, n=6):
        """True if n instructions decode without hitting padding/invalid bytes."""
        for _ in range(n):
            insn = self.decode(a)
            if insn is None or insn.mnemonic in ("int3",) or insn.bytes == b"\x00\x00":
                return False
            if insn.mnemonic in ("ret", "jmp"):
                return True
            a += insn.size
        return True

    def decode(self, a):
        if not hasattr(self, "_dec"):
            self._dec = {}
        if a in self._dec:
            return self._dec[a]
        data = self.img.read(a, 16)
        insn = next(self.md.disasm(data, a, 1), None)
        self._dec[a] = insn
        return insn

    def noreturn(self, t):
        # IAT-based ExitProcess etc are indirect; direct no-return detection kept minimal
        return t in self._noreturn

    def fname(self, a):
        return f"f_{a:08x}"

    def run(self):
        os.makedirs(self.outdir, exist_ok=True)
        entries = sorted(self.func_entries)
        hdr = ['#include "cpu.h"']
        hdr += [f"void {self.fname(a)}(CPU *c);" for a in entries]
        hdr += [f"void {n}(CPU *c);" for n in sorted(set(self.hooks.values()))]
        open(os.path.join(self.outdir, "funcs.h"), "w").write("\n".join(hdr) + "\n")
        chunk, n_chunk, out = [], 0, []
        notes = []
        insn_total = 0
        for i, a in enumerate(entries):
            ft = FuncTranslator(self, a, self.cjumps)
            lines = ft.translate()
            insn_total += len(ft.ins)
            notes += [f"{a:#x}: {n}" for n in ft.notes]
            chunk.extend(lines)
            chunk.append("")
            if len(chunk) > 40000 or i == len(entries) - 1:
                path = os.path.join(self.outdir, f"code_{n_chunk:03d}.c")
                open(path, "w").write('#include "funcs.h"\n#pragma clang diagnostic ignored "-Wunused-label"\n\n' + "\n".join(chunk))
                chunk = []
                n_chunk += 1
        # dispatch table
        tbl = ['#include "funcs.h"', "typedef struct { uint32_t a; GuestFn f; } FnEnt;",
               f"const unsigned g_fn_count = {len(entries)};", "const FnEnt g_fn_table[] = {"]
        tbl += [f"  {{0x{a:x}u, {self.fname(a)}}}," for a in entries]
        tbl += ["};", f"const uint32_t g_entry_point = 0x{self.entry_point:x}u;"]
        open(os.path.join(self.outdir, "table.c"), "w").write("\n".join(tbl) + "\n")
        # imports
        imp = ["#include <stdint.h>", "typedef struct { uint32_t slot; const char *name; } ImpEnt;",
               f"const unsigned g_imp_count = {len(self.img.imports)};", "const ImpEnt g_imp_table[] = {"]
        imp += [f'  {{0x{s:x}u, "{n}"}},' for s, n in sorted(self.img.imports.items())]
        imp += ["};"]
        open(os.path.join(self.outdir, "imports.c"), "w").write("\n".join(imp) + "\n")
        print(f"functions: {len(entries)}  instructions: {insn_total}  files: {n_chunk}")
        print("unsupported mnemonics:", self.unsupported.most_common())
        print("calls to non-entries:", len(self.missing_calls))
        open(os.path.join(self.outdir, "notes.txt"), "w").write("\n".join(notes))
        print("discovery notes:", len(notes))


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(__file__))
    Recompiler(sys.argv[1], sys.argv[2], sys.argv[3]).run()
