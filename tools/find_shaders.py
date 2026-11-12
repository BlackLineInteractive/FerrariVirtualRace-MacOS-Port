"""Find D3D9 shader bytecode blobs (version token + CTAB comment) embedded in a PE."""
import sys, struct, collections, pefile
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
found = collections.Counter(); blobs = []
for s in pe.sections:
    d = s.get_data()
    for off in range(0, len(d) - 12, 4):
        tok, = struct.unpack_from("<I", d, off)
        if tok >> 16 in (0xFFFE, 0xFFFF) and (tok & 0xFFFF) in (0x0200, 0x0201, 0x0300, 0x0101, 0x0104, 0x0103):
            nxt, = struct.unpack_from("<I", d, off + 4)
            if nxt & 0xFFFF == 0xFFFE and d[off + 8:off + 12] == b"CTAB":
                kind = ("vs" if tok >> 16 == 0xFFFE else "ps") + f"_{(tok>>8)&0xff}_{tok&0xff}"
                found[kind] += 1
                blobs.append((base + s.VirtualAddress + off, kind))
print(dict(found), "total", len(blobs))
