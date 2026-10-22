# Resolve IV-SDK .NET CE addresses/patterns against a decrypted GTAIV.exe image and mark
# which ones FusionFix already reaches.
# usage: compare.py <image.bin> <12059.ini> <Patterns.ini> <fusionfix/source> <out.tsv>
import os, re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

img_path, addr_ini, pat_ini, src_dir, out_path = sys.argv[1:6]
img = open(img_path, 'rb').read()
BASE = 0x400000
TEXT = (0x1000, 0xa73000)
LO, HI = BASE + 0x1000, BASE + len(img)
cs = Cs(CS_ARCH_X86, CS_MODE_32)
cs.detail = True


def u32(rva):
    return struct.unpack_from('<I', img, rva)[0]


def compile_pat(p):
    out = b''
    for t in p.split():
        out += b'.' if t.startswith('?') else re.escape(bytes([int(t, 16)]))
    return re.compile(out, re.S)


def find(p, limit=3):
    rx = compile_pat(p)
    res = []
    for m in rx.finditer(img, TEXT[0], TEXT[1]):
        res.append(m.start())
        if len(res) >= limit:
            break
    return res


def refs(rva, length):
    """globals and call/jump targets referenced by the instructions in [rva, rva+length)"""
    g, f = set(), set()
    for ins in cs.disasm(img[rva:rva + length + 16], BASE + rva):
        if ins.address >= BASE + rva + length:
            break
        for op in ins.operands:
            if op.type == X86_OP_IMM and LO <= op.imm < HI:
                (f if ins.mnemonic.startswith(('call', 'j')) else g).add(op.imm)
            elif op.type == X86_OP_MEM and op.mem.base == 0 and LO <= op.mem.disp < HI:
                g.add(op.mem.disp)
    return g, f


# --- what FusionFix reaches -------------------------------------------------
our_globals, our_funcs, our_sites, literals = set(), set(), set(), set()
pat_rx = re.compile(r'(?:hook::pattern|find_pattern|get_pattern)\s*\(\s*"([0-9A-Fa-f? ]+)"')
n_ours = n_found = 0
for root, _, files in os.walk(src_dir):
    for fn in files:
        if not fn.endswith(('.ixx', '.cpp', '.hpp', '.hxx', '.h')):
            continue
        text = open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'0x([0-9A-Fa-f]{6,8})\b', text):
            v = int(m.group(1), 16)
            if LO <= v < HI:
                literals.add(v)
        for m in pat_rx.finditer(text):
            n_ours += 1
            for site in find(m.group(1), 4):
                n_found += 1
                ln = len(m.group(1).split())
                g, f = refs(site, ln + 24)
                our_globals |= g
                our_funcs |= f
                our_sites.add(BASE + site)
print(f'our patterns: {n_ours}, matches {n_found}, globals {len(our_globals)}, funcs {len(our_funcs)}, literals {len(literals)}')


def ours(addrs):
    hits = []
    for a in addrs:
        if a in literals:
            hits.append('literal')
        if a in our_globals:
            hits.append('global')
        if a in our_funcs:
            hits.append('call')
        if any(abs(a - s) < 0x40 for s in our_sites):
            hits.append('site')
    return ','.join(sorted(set(hits)))


def parse_ini(path):
    sec, out = None, []
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if not line or line.startswith(';'):
            continue
        if line.startswith('['):
            sec = line.strip('[]')
            continue
        if '=' in line:
            k, v = line.split('=', 1)
            out.append((sec, k.strip(), v.strip()))
    return out


rows = {}

# --- 12059.ini: RVA of an operand (data) or of a function ----------------------
for sec, key, val in parse_ini(addr_ini):
    if not val.lower().startswith('0x'):
        continue
    rva = int(val, 16)
    if rva + 4 > len(img):
        continue
    d = u32(rva)
    prev = img[rva - 1]
    if prev in (0xE8, 0xE9):
        kind, target = 'func', (BASE + rva + 4 + d) & 0xffffffff
    elif prev in (0xCC, 0xC3, 0x90) and rva % 16 == 0:
        kind, target = 'func', BASE + rva
    else:
        # the RVA is not always the operand itself (sometimes the ModRM byte or the last byte of
        # the instruction before): take the global of the instruction that covers it, or the next one
        kind, target = None, None
        for back in range(0, 12):
            for ins in cs.disasm(img[rva - back:rva - back + 32], BASE + rva - back):
                if ins.address > BASE + rva + 1:
                    break
                g = [op.mem.disp for op in ins.operands if op.type == X86_OP_MEM and op.mem.base == 0 and LO <= op.mem.disp < HI]
                g += [op.imm for op in ins.operands if op.type == X86_OP_IMM and LO <= op.imm < HI and not ins.mnemonic.startswith(('call', 'j'))]
                if g and ins.address + ins.size > BASE + rva:
                    kind, target = 'data', g[0]
                    break
            if kind:
                break
        if not kind:
            kind, target = ('data', d) if LO <= d < HI else ('func', BASE + rva)
    rows[(sec, key)] = dict(sec=sec, key=key, kind=kind, ini=target, pat='', n='')

# --- Patterns.ini: the CE pattern is the second one when two are given ---------
for sec, key, val in parse_ini(pat_ini):
    pats = [p.strip() for p in val.split(',') if p.strip()]
    if not pats or not re.fullmatch(r'[0-9A-Fa-f? ]+', pats[-1]):
        continue
    p = pats[1] if len(pats) > 1 else pats[0]
    hits = find(p, 3)
    r = rows.setdefault((sec, key), dict(sec=sec, key=key, kind='', ini=None, pat='', n=''))
    r['n'] = str(len(hits)) + ('+' if len(hits) == 3 else '')
    if len(hits) == 1:
        site = hits[0]
        g, f = refs(site, max(8, len(p.split())))
        if img[site] == 0xE8:
            r['pat'] = '%#x' % ((BASE + site + 5 + u32(site + 1)) & 0xffffffff)
            r['kind'] = r['kind'] or 'func'
        elif r['kind'] == 'data' or (not r['kind'] and g and key[:1] in ('m', 's', 'g', 'T') and not key.startswith(('Set', 'Get'))):
            r['pat'] = ' '.join('%#x' % x for x in sorted(g)[:3])
            r['kind'] = r['kind'] or 'data?'
        else:
            r['pat'] = '%#x' % (BASE + site)
            r['kind'] = r['kind'] or 'func?'

with open(out_path, 'w') as o:
    o.write('class\tname\tkind\taddr_from_12059\tfrom_pattern\tmatches\twe_have\n')
    for r in sorted(rows.values(), key=lambda r: (r['sec'] or '', r['key'])):
        addrs = set()
        if r['ini']:
            addrs.add(r['ini'])
        for t in r['pat'].split():
            addrs.add(int(t, 16))
        o.write('\t'.join([r['sec'] or '', r['key'], r['kind'], '%#x' % r['ini'] if r['ini'] else '',
                           r['pat'], r['n'], ours(addrs)]) + '\n')
print('rows', len(rows))
