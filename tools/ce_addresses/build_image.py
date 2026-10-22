# Rebuild a decrypted, un-relocated GTAIV.exe image (base 0x400000) from a full minidump.
# usage: build_image.py <exe> <dump> <out.bin>
import struct, sys
import pefile

exe_path, dump_path, out_path = sys.argv[1:4]
pe = pefile.PE(exe_path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
size = pe.OPTIONAL_HEADER.SizeOfImage
static_img = bytearray(pe.get_memory_mapped_image())
static_img = static_img[:size] + bytes(max(0, size - len(static_img)))

d = open(dump_path, 'rb').read()
sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', d, 0)
streams = {}
for i in range(nstreams):
    t, sz, rva = struct.unpack_from('<III', d, dir_rva + i * 12)
    streams[t] = (sz, rva)

# ModuleListStream (4): find GTAIV.exe base
sz, rva = streams[4]
n = struct.unpack_from('<I', d, rva)[0]
base = None
for i in range(n):
    off = rva + 4 + i * 108
    b, msize = struct.unpack_from('<QI', d, off)
    name_rva = struct.unpack_from('<I', d, off + 20)[0]
    ln = struct.unpack_from('<I', d, name_rva)[0]
    name = d[name_rva + 4:name_rva + 4 + ln].decode('utf-16le')
    if name.lower().endswith('gtaiv.exe'):
        base = b
assert base, 'GTAIV.exe not in module list'

# Memory64ListStream (9)
sz, rva = streams[9]
cnt, data_rva = struct.unpack_from('<QQ', d, rva)
img = bytearray(size)
have = bytearray(size)
pos = data_rva
for i in range(cnt):
    start, ln = struct.unpack_from('<QQ', d, rva + 16 + i * 16)
    lo, hi = max(start, base), min(start + ln, base + size)
    if lo < hi:
        img[lo - base:hi - base] = d[pos + lo - start:pos + hi - start]
        have[lo - base:hi - base] = b'\1' * (hi - lo)
    pos += ln

delta = (base - 0x400000) & 0xffffffff
# undo relocations
for block in pe.DIRECTORY_ENTRY_BASERELOC:
    for e in block.entries:
        if e.type == 3:
            r = e.rva
            if have[r] and have[r + 3]:
                v = struct.unpack_from('<I', img, r)[0]
                struct.pack_into('<I', img, r, (v - delta) & 0xffffffff)

# writable data comes from the static file (runtime values are noise); code + rdata from the dump
for s in pe.sections:
    name = s.Name.rstrip(b'\0')
    a, b_ = s.VirtualAddress, s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData)
    if name not in (b'.text', b'.rdata'):
        img[a:b_] = static_img[a:b_]
    else:
        for k in range(a, min(b_, size), 0x1000):
            if not have[k]:
                img[k:k + 0x1000] = static_img[k:k + 0x1000]
missing = sum(1 for k in range(0x1000, 0xa73000, 0x1000) if not have[k])
open(out_path, 'wb').write(img)
print(f'base {base:#x} delta {delta:#x}, .text pages missing from dump: {missing}')
