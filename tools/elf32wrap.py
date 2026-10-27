#!/usr/bin/env python3
"""A 64-bit tOS kernel as the 32-bit ELF a Multiboot (1) loader takes: every loadable segment
at its physical address (vaddr = paddr), the entry at the 32-bit start.
    elf32wrap.py kernel64.elf tOS.elf"""
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
assert d[:4] == b'\x7fELF' and d[4] == 2
entry, phoff = struct.unpack_from('<QQ', d, 24)
phentsize, phnum = struct.unpack_from('<HH', d, 54)
segs = []
for i in range(phnum):
    p_type, p_flags, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
    if p_type != 1 or p_memsz == 0: continue
    assert p_paddr < 1 << 32 and p_paddr + p_memsz < 1 << 32, hex(p_paddr)
    segs.append((p_flags, p_paddr, d[p_off:p_off + p_filesz], p_memsz))
if entry >= 1 << 32:                     # the entry must be the 32-bit start (physical)
    raise SystemExit('entry is not physical: %x' % entry)
hdr = 52; ph = 32 * len(segs)
out = bytearray(hdr + ph)
off = (hdr + ph + 4095) & ~4095
body = bytearray()
phs = b''
for flags, paddr, data, memsz in segs:
    pad = (-(off + len(body)) + paddr) % 4096
    body += b'\0' * pad
    o = off + len(body)
    phs += struct.pack('<IIIIIIII', 1, o, paddr, paddr, len(data), memsz, flags, 4096)
    body += data
ident = b'\x7fELF\x01\x01\x01' + b'\0' * 9
struct.pack_into('<16sHHIIIIIHHHHHH', out, 0, ident, 2, 3, 1, entry, hdr, 0, 0, 52, 32, len(segs), 0, 0, 0)
out[hdr:hdr + ph] = phs
out += b'\0' * (off - len(out))
out += body
open(dst, 'wb').write(out)
print('%d segments, entry %#x, %d bytes' % (len(segs), entry, len(out)))
