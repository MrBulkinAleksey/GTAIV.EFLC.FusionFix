import os
import struct, sys, zlib
from Crypto.Cipher import AES
exe = open(os.environ.get('GTAIV_EXE', 'bin/GTAIV.exe'),'rb').read()
KEY = exe[0xc5b73c:0xc5b73c+32]
def dec(b):
    n = len(b)//16*16; c = AES.new(KEY, AES.MODE_ECB); x = b[:n]
    for _ in range(16): x = c.decrypt(x)
    return x + b[n:]
def toc(path):
    f = open(path,'rb'); hdr = dec(f.read(16))
    magic, ver, count, tocsize = struct.unpack('<4I', hdr)
    assert magic == 0xA94E2A52, hex(magic)
    rest = f.read(4); entsz, unk = struct.unpack('<HH', rest)
    t = dec(f.read(tocsize))
    ents = []
    for i in range(count):
        flags, rtype, off, blocks, pad = struct.unpack_from('<IIIHH', t, i*16)
        ents.append([flags, rtype, off*2048, blocks*2048 - (pad & 0x7ff)])
    names = t[count*16:].split(b'\0')
    for e, n in zip(ents, names): e.append(n.decode())
    return ents
if __name__ == '__main__':
    e = toc(sys.argv[1]); print(len(e))
    for x in e[:400]: print(x[4], hex(x[0]), x[1], x[3])
