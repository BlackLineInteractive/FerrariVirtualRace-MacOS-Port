import sys, capstone, pefile
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
data = pe.get_memory_mapped_image()[a - base:a - base + n * 15]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i, ins in enumerate(md.disasm(data, a)):
    if i >= n: break
    print(f"{ins.address:08x}: {ins.bytes.hex():20} {ins.mnemonic} {ins.op_str}")
