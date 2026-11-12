"""Dump PE layout, imports, relocations and instruction-class stats for a 32-bit exe."""
import sys, collections
import pefile, capstone

path = sys.argv[1]
pe = pefile.PE(path)
oh = pe.OPTIONAL_HEADER
print(f"ImageBase=0x{oh.ImageBase:x} Entry=0x{oh.ImageBase + oh.AddressOfEntryPoint:x} "
      f"Linker={oh.MajorLinkerVersion}.{oh.MinorLinkerVersion} DllChars=0x{oh.DllCharacteristics:x}")
print("Relocs:", hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"), " TLS:", hasattr(pe, "DIRECTORY_ENTRY_TLS"))
for s in pe.sections:
    print(f"  {s.Name.rstrip(b'\0').decode():8} va=0x{oh.ImageBase + s.VirtualAddress:08x} "
          f"vsz=0x{s.Misc_VirtualSize:x} raw=0x{s.SizeOfRawData:x} chars=0x{s.Characteristics:x}")

print("\nIMPORTS")
total = 0
for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    names = [(i.name.decode() if i.name else f"#{i.ordinal}") for i in e.imports]
    total += len(names)
    print(f"  {e.dll.decode()} ({len(names)}): {' '.join(names)}")
print("total imports:", total)

# instruction class stats over .text (linear sweep, rough)
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
code = text.get_data()
base = oh.ImageBase + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
groups = collections.Counter()
mn = collections.Counter()
n = 0
for ins in md.disasm_lite(code, base):
    n += 1
    m = ins[2]
    mn[m] += 1
    if m.startswith("f"): groups["x87"] += 1
    elif m.endswith(("ps", "ss", "pd", "sd")) or m.startswith(("cvt", "movaps", "movups", "xorps")): groups["sse"] += 1
    elif m.startswith(("p", "movq", "movd", "emms")): groups["mmx/int-simd"] += 1
print(f"\n.text linear-sweep instructions: {n}")
print("classes:", dict(groups))
print("distinct mnemonics:", len(mn))
print("top 60:", mn.most_common(60))
