import os
import struct, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ex import get
class R:
    def __init__(s, name):
        m, t, fl, s.d = get(name)
        s.sys = (fl & 0x7ff) << (((fl >> 11) & 0xf) + 8)
    def off(s, v):
        if v >> 28 == 5: return v - 0x50000000
        if v >> 28 == 6: return s.sys + v - 0x60000000
        raise ValueError(hex(v))
    def u32(s, o): return struct.unpack_from('<I', s.d, o)[0]
    def u16(s, o): return struct.unpack_from('<H', s.d, o)[0]
    def cstr(s, v): o = s.off(v); return s.d[o:s.d.index(b'\0', o)].decode('latin1')
def parse(name):
    r = R(name); d = r.d
    hdr_draw = r.off(r.u32(0xb4))
    sk = r.off(r.u32(hdr_draw + 0xc)); bones = r.off(r.u32(sk)); nb = r.u16(sk + 0x14 + 2) if False else None
    # bone names: walk bone array until name ptr invalid
    bnames = []; o = bones
    while True:
        try: nm = r.cstr(r.u32(o))
        except Exception: break
        if not re.fullmatch(r'[a-z0-9_]+', nm): break
        bnames.append((nm, struct.unpack_from('<3f', d, o + 0x20))); o += 0xe0
    sg = r.off(r.u32(hdr_draw + 8)); sarr = r.off(r.u32(sg + 8)); ns = r.u16(sg + 12)
    shaders = []
    for i in range(ns):
        so = r.off(r.u32(sarr + 4 * i))
        sname = r.cstr(r.u32(so + 0x44))
        np_ = r.u32(so + 0x1c) & 0xff
        pp = r.off(r.u32(so + 0x14)); tex = []
        types = r.off(r.u32(so + 0x34)) if r.u32(so+0x34) else None
        for k in range(np_):
            v = r.u32(pp + 4 * k)
            if v >> 28 == 5:
                to = r.off(v)
                try:
                    nmv = r.u32(to + 0x18)
                    if nmv >> 28 == 5: tex.append(r.cstr(nmv))
                except Exception: pass
        shaders.append((sname, tex))
    coll = r.off(r.u32(hdr_draw + 0x40)); marr = r.off(r.u32(coll)); nm_ = r.u16(coll + 4)
    out = []
    for mi in range(nm_):
        m = r.off(r.u32(marr + 4 * mi))
        garr = r.off(r.u32(m + 4)); ng = r.u16(m + 8)
        sidx = r.off(r.u32(m + 0x10))
        for gi in range(ng):
            g = r.off(r.u32(garr + 4 * gi))
            vb = r.off(r.u32(g + 0xc)); nv = r.u16(vb + 4); stride = r.u32(vb + 0xc)
            vdata = r.off(r.u32(vb + 8)); decl = r.off(r.u32(vb + 0x10))
            flags = r.u32(decl)
            bmap = r.off(r.u32(g + 0x38)) if r.u32(g+0x38) else None; nbm = r.u16(g + 0x3e)
            ib = r.off(r.u32(g + 0x1c)); ni = r.u32(ib + 4); idata = r.off(r.u32(ib + 8))
            verts = []
            for k in range(nv):
                vo = vdata + k * stride
                x, y, z = struct.unpack_from('<3f', d, vo)
                bi = None
                if flags & 4 and bmap is not None:
                    w = d[vo + 12:vo + 16]; ix = d[vo + 16:vo + 20]
                    j = max(range(4), key=lambda q: w[q]); bi = r.u16(bmap + 2 * ix[j]) if ix[j] < nbm else None
                verts.append((x, y, z, bi))
            idx = struct.unpack_from('<%dH' % ni, d, idata)
            out.append(dict(shader=shaders[r.u16(sidx + 2 * gi)], verts=verts, idx=idx))
    return bnames, out
if __name__ == '__main__':
    bn, geoms = parse(sys.argv[1])
    for i, g in enumerate(geoms):
        v = g['verts']; xs = [p[0] for p in v]; ys = [p[1] for p in v]; zs = [p[2] for p in v]
        from collections import Counter
        bc = Counter(bn[p[3]][0] if p[3] is not None and p[3] < len(bn) else '?' for p in v).most_common(4)
        print(i, g['shader'][0], g['shader'][1][:3], len(v), 'x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f' % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)), bc)
