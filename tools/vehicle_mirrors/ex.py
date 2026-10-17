import os
import sys, struct, zlib, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from img3 import toc
path = os.environ.get('VEHICLES_IMG', 'log/vehicles.img')
f = open(path,'rb')
def get(name):
    for e in toc(path):
        if e[4] == name:
            f.seek(e[2]); d = f.read(e[3])
            magic, rtype, flags = struct.unpack_from('<III', d)
            return magic, rtype, flags, zlib.decompress(d[12:])
if __name__ == '__main__':
    m, t, fl, d = get(sys.argv[1])
    print(hex(m), t, hex(fl), len(d))
    open(sys.argv[2],'wb').write(d)
