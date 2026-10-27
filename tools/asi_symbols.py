#!/usr/bin/env python3
"""Names addresses of GTAIV.EFLC.FusionFix.asi from the PDB that CI embeds in it (release.bat's EmbedPDB: zlib data,
its sizes unpacked and packed as two u64, "PDB_EMBED_V1" and four zeros), without the game or DbgHelp: for a profile or
crash log that shows FusionFix's functions as GTAIV.EFLC.FusionFix.asi+offset, given the ASI of the same build.

    tools/asi_symbols.py GTAIV.EFLC.FusionFix.asi 0x1919e0 63bb0 ...

Offsets are from the module's base (the profiler's asi+offset). The ASI of a CI run is its "GTAIV.EFLC.FusionFix.asi"
artifact; the run is the one whose build time matches the ASI's link time (its PE TimeDateStamp, printed here). Names
are taken from the public symbols and the procedures of each module's symbols (static functions and lambdas too).
"""

import bisect
import struct
import sys
import zlib


def embedded_pdb(asi):
    if asi[-16:-4] != b"PDB_EMBED_V1":
        sys.exit("no embedded PDB in this ASI (a local build has its .pdb next to it)")
    raw, packed = struct.unpack_from("<QQ", asi, len(asi) - 32)
    pdb = zlib.decompress(asi[len(asi) - 32 - packed:len(asi) - 32])
    if len(pdb) != raw:
        sys.exit("the embedded PDB is damaged")
    return pdb


def symbols(d):
    # MSF 7: the superblock, the stream directory through its block map
    bs, _, _, dirbytes, _, bmaddr = struct.unpack_from("<6I", d, 32)
    dirblocks = struct.unpack_from("<%dI" % ((dirbytes + bs - 1) // bs), d, bmaddr * bs)
    directory = b"".join(d[b * bs:(b + 1) * bs] for b in dirblocks)[:dirbytes]
    nstreams = struct.unpack_from("<I", directory, 0)[0]
    sizes = struct.unpack_from("<%dI" % nstreams, directory, 4)
    pos, blocks = 4 + 4 * nstreams, []
    for size in sizes:
        n = 0 if size == 0xFFFFFFFF else (size + bs - 1) // bs
        blocks.append(struct.unpack_from("<%dI" % n, directory, pos))
        pos += 4 * n

    def stream(i):
        return b"".join(d[b * bs:(b + 1) * bs] for b in blocks[i])[:sizes[i]]

    # DBI: the symbol records, the modules, the section headers (optional debug header, index 5)
    dbi = stream(3)
    fields = struct.unpack_from("<iIIHHHHHHiiiiiIiiHHI", dbi, 0)
    symrec, modsz, scsz, smsz, srcsz, tsmsz, dbgsz, ecsz = fields[7], fields[9], fields[10], fields[11], fields[12], fields[13], fields[15], fields[16]
    dbg = struct.unpack_from("<%dH" % (dbgsz // 2), dbi, 64 + modsz + scsz + smsz + srcsz + tsmsz + ecsz)
    sect = stream(dbg[5])
    sections = [struct.unpack_from("<I", sect, k * 40 + 12)[0] for k in range(len(sect) // 40)]

    found = []

    def scan(buf, p):
        while p + 4 <= len(buf):
            length, kind = struct.unpack_from("<HH", buf, p)
            if length < 2:
                break
            rec = buf[p + 4:p + 2 + length]
            if kind == 0x110E:  # S_PUB32
                _, off, seg = struct.unpack_from("<IIH", rec, 0)
                name = rec[10:]
            elif kind in (0x110F, 0x1110, 0x1147, 0x1148):  # S_LPROC32, S_GPROC32 and their _ID forms
                off, seg = struct.unpack_from("<IH", rec, 28)
                name = rec[35:]
            else:
                p += 2 + length
                continue
            if 1 <= seg <= len(sections):
                found.append((sections[seg - 1] + off, name.split(b"\0")[0].decode("latin1")))
            p += 2 + length

    scan(stream(symrec), 0)
    p = 64
    while p < 64 + modsz:
        modstream, symbytes = struct.unpack_from("<HxxI", dbi, p + 34)
        names = dbi[p + 64:].split(b"\0", 2)
        if modstream != 0xFFFF:
            scan(stream(modstream)[:symbytes], 4)
        p = (p + 64 + len(names[0]) + 1 + len(names[1]) + 1 + 3) & ~3
    found.sort()
    return found


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    asi = open(sys.argv[1], "rb").read()
    pe = struct.unpack_from("<I", asi, 0x3C)[0]
    print("link time %08x" % struct.unpack_from("<I", asi, pe + 8)[0])
    found = symbols(embedded_pdb(asi))
    starts = [a for a, _ in found]
    for text in sys.argv[2:]:
        address = int(text, 16)
        i = bisect.bisect_right(starts, address) - 1
        if i < 0:
            print("%#x ?" % address)
            continue
        print("%#x %s+%#x" % (address, found[i][1], address - found[i][0]))


if __name__ == "__main__":
    main()
