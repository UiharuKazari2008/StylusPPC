# IMGFS reader for WM 6.5 (HTC HD2) partitions. Written from observed structure + [MS-XCA] spec.
#   python -I imgfs.py <imgfs.bin> list
#   python -I imgfs.py <imgfs.bin> get <outdir> <name-substring|*> [...]
# Modules are rebuilt into PE files (e32_rom/o32_rom -> PE32 ARM).
import sys, struct, os

T_FILE, T_PLAIN, T_NAME, T_SECT = 0xFFFFFEFE, 0xFFFFF6FE, 0xFFFFFEFB, 0xFFFFF6FD
SIG_HDR = 0x2F5314CE


def xph_decompress(src, usize):
    """[MS-XCA] LZ77+Huffman, single 64K block is enough for 4K chunks."""
    out = bytearray()
    pos = 0
    n = len(src)

    def r16(p):
        return src[p] | (src[p + 1] << 8) if p + 1 < n else (src[p] if p < n else 0)

    while len(out) < usize:
        lens = []
        for i in range(256):
            b = src[pos + i]
            lens.append(b & 15); lens.append(b >> 4)
        pos += 256
        # canonical table, 15-bit lookup
        table = [None] * 32768
        code = 0
        start = 0
        for L, sym in sorted((lens[s], s) for s in range(512) if lens[s]):
            span = 1 << (15 - L)
            table[start:start + span] = [(sym, L)] * span
            start += span
        nb = (r16(pos) << 16) | r16(pos + 2); pos += 4
        extra = 16
        block_end = len(out) + 65536
        while len(out) < usize and len(out) < block_end:
            sym, L = table[nb >> 17]
            nb = (nb << L) & 0xFFFFFFFF; extra -= L
            if extra < 0:
                nb |= r16(pos) << (-extra); extra += 16; pos += 2
            if sym < 256:
                out.append(sym); continue
            sym -= 256
            length = sym & 15; olog = sym >> 4
            if length == 15:
                length = src[pos]; pos += 1
                if length == 255:
                    length = r16(pos); pos += 2
                    if length == 0:
                        length = struct.unpack_from('<I', src, pos)[0]; pos += 4
                    length -= 15
                length += 15
            length += 3
            off = (((nb >> 1) | 0x80000000) >> (31 - olog))
            nb = (nb << olog) & 0xFFFFFFFF; extra -= olog
            if extra < 0:
                nb |= r16(pos) << (-extra); extra += 16; pos += 2
            s = len(out) - off
            if s < 0:
                raise ValueError('bad offset')
            for k in range(length):
                out.append(out[s + k])
    return bytes(out[:usize])


class Imgfs:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        self.bph = struct.unpack_from('<I', d, 0x1C)[0]      # bytes per header entry (0x34)
        self.sect = struct.unpack_from('<I', d, 0x24)[0]     # sector size
        self.comp = d[0x2C:0x30].rstrip(b'\0').decode()
        self.entries = {}
        off = self.sect                                        # first header block follows boot sector
        while off:
            sig, nxt = struct.unpack_from('<II', d, off)
            assert sig == SIG_HDR, hex(off)
            p = off + 8
            while p + self.bph <= off + self.sect:
                self.entries[p] = d[p:p + self.bph]
                p += self.bph
            off = nxt
        self.files = []
        for p, e in self.entries.items():
            if struct.unpack_from('<I', e, 0)[0] in (T_FILE, T_PLAIN):
                self.files.append(self.parse_file(e))
        self.files.sort(key=lambda f: f['name'].lower())

    def name(self, e, at):
        cch, flags = struct.unpack_from('<HH', e, at)
        if cch <= 4:
            return e[at + 4:at + 4 + cch * 2].decode('utf-16le')
        noff = struct.unpack_from('<I', e, at + 8)[0]
        if struct.unpack_from('<I', self.d, noff)[0] == T_NAME:
            noff += 4                                          # name entry; very long names point at raw text
        n = self.d[noff:noff + cch * 2]
        return n.decode('utf-16le')

    def parse_file(self, e):
        f = {}
        f['first_sect'] = struct.unpack_from('<I', e, 8)[0]
        f['name'] = self.name(e, 0x0C)
        f['size'], f['attr'] = struct.unpack_from('<II', e, 0x18)
        f['tbl'], f['tblsize'] = struct.unpack_from('<II', e, 0x2C)
        f['sects'] = []
        s = f['first_sect']
        while s:
            se = self.d[s:s + self.bph]
            typ, _, nxt = struct.unpack_from('<III', se, 0)
            assert typ == T_SECT
            nm = self.name(se, 0x0C)
            size, tbl, tsz = struct.unpack_from('<III', se, 0x18)
            f['sects'].append({'name': nm, 'size': size, 'tbl': tbl, 'tblsize': tsz})
            s = nxt
        return f

    def stream(self, size, tbl, tblsize):
        out = bytearray()
        p = tbl
        while len(out) < size and p < tbl + tblsize:
            cs, us, off = struct.unpack_from('<HHI', self.d, p)
            p += 8
            if cs == 0 and us == 0:
                continue
            raw = self.d[off:off + cs]
            out += raw if cs == us else xph_decompress(raw, us)
        return bytes(out[:size])

    def is_module(self, f):
        return bool(f['sects'])


ROM_EXTRA = 9
RAM_DLL_BASE = 0x10000000


def rebuild_relocs(sects, vbase, vsize, reloc_rva, newbase):
    """IMGFS modules keep their relocations in romimage's compressed format, which the PE loader
    can't read, and romimage may have placed writable sections at o32_realaddr instead of
    vbase+rva. Recover the fixups instead: ARMv4 code keeps every absolute address in literal pools
    (found via PC-relative LDRs), and data sections hold pointers as aligned words. Any such word
    inside the image (or a moved section) is a HIGHLOW fixup; moved targets are pointed back at
    vbase+rva. Every fixup is then rebased to newbase. Returns standard IMAGE_BASE_RELOCATION blocks."""
    moved = [(real, ovs, rva) for ovs, rva, data, real, fl in sects
             if real and real != vbase + rva and rva != reloc_rva]
    sites = []
    for s in sects:
        ovs, va, data, real, flags = s
        if va == reloc_rva:
            continue
        n = len(data) & ~3
        if flags & 0x20:                                       # code: only literal-pool slots
            slots = set()
            for o in range(0, n, 4):
                w = struct.unpack_from('<I', data, o)[0]
                if (w & 0x0F7F0000) == 0x051F0000 and (w >> 28) != 0xF:   # LDR Rd, [pc, #+/-imm]
                    imm = w & 0xFFF
                    t = o + 8 + (imm if w & 0x00800000 else -imm)
                    if 0 <= t <= n - 4 and t % 4 == 0:
                        slots.add(t)
            offs = sorted(slots)
        else:
            offs = range(0, n, 4)
        for o in offs:
            v = struct.unpack_from('<I', data, o)[0]
            for real, mvs, rva in moved:
                if real <= v < real + max(mvs, 1):
                    struct.pack_into('<I', data, o, vbase + rva + v - real)
                    sites.append(va + o)
                    break
            else:
                if vbase <= v < vbase + vsize:
                    sites.append(va + o)
    for r in sites:
        for s in sects:
            if s[1] <= r < s[1] + len(s[2]):
                o = r - s[1]
                struct.pack_into('<I', s[2], o, (struct.unpack_from('<I', s[2], o)[0] + newbase - vbase) & 0xFFFFFFFF)
                break
    blocks = bytearray()
    pages = {}
    for r in sorted(sites):
        pages.setdefault(r & ~0xFFF, []).append(0x3000 | (r & 0xFFF))
    for page, ents in sorted(pages.items()):
        if len(ents) & 1:
            ents.append(0)
        blocks += struct.pack('<II', page, 8 + 2 * len(ents)) + struct.pack('<%dH' % len(ents), *ents)
    return bytes(blocks), len(sites)


def rebuild_pe(img, f):
    hdr = img.stream(f['size'], f['tbl'], f['tblsize'])
    (objcnt, imgflags, entry, vbase, smaj, smin, stack, vsize, s14r, s14s, ts) = struct.unpack_from('<HHIIHHIIIII', hdr, 0)
    units = [struct.unpack_from('<II', hdr, 0x24 + i * 8) for i in range(ROM_EXTRA)]
    subsys = struct.unpack_from('<H', hdr, 0x6C)[0]
    o32 = [struct.unpack_from('<6I', hdr, 0x70 + i * 24) for i in range(objcnt)]
    sects = []
    for i, (ovs, rva, psz, dptr, real, flags) in enumerate(o32):
        data = img.stream(f['sects'][i]['size'], f['sects'][i]['tbl'], f['sects'][i]['tblsize']) if i < len(f['sects']) else b''
        sects.append([ovs, rva, bytearray(data), real, flags & ~0x2000])
    reloc_rva = units[5][0] if units[5][1] else None
    # A RAM DLL whose preferred base lies in slot 1 (0x02000000-0x03FFFFFF, XIP DLL code) is loaded
    # elsewhere but never relocated by the WM 6.5 loader: rebase DLLs to an ordinary RAM-DLL base.
    rombase = vbase
    if f['name'].lower().endswith(('.dll', '.dll1', '.cpl')):
        vbase = RAM_DLL_BASE
    relocs, nfix = rebuild_relocs(sects, rombase, vsize, reloc_rva, vbase)
    rsec = [s for s in sects if s[1] == reloc_rva]
    if rsec:
        rsec[0][0], rsec[0][2] = max(len(relocs), 4), bytearray(relocs)
        rsec[0][4] = 0x42000040    # romimage's bit 0x2 marks its compressed format: the loader would skip ours
    else:                                                      # stripped: append a new .reloc section
        reloc_rva = (vsize + 0xFFF) & ~0xFFF
        sects.append([max(len(relocs), 4), reloc_rva, bytearray(relocs), 0, 0x42000040])
        vsize = reloc_rva + ((len(relocs) + 0xFFF) & ~0xFFF)
        objcnt = len(sects)
    units[5] = (reloc_rva, len(relocs))
    # names
    def secname(rva, flags):
        for idx, nm in ((2, b'.rsrc'), (3, b'.pdata'), (5, b'.reloc')):
            if units[idx][0] == rva and units[idx][1]:
                return nm
        if flags & 0x20: return b'.text'
        if flags & 0x80000000: return b'.data'
        return b'.rdata'
    fa = 0x200
    nsec = len(sects)
    hsize = (0x80 + 4 + 20 + 224 + 40 * nsec + fa - 1) // fa * fa
    pe = bytearray(hsize)
    pe[0:2] = b'MZ'; struct.pack_into('<I', pe, 0x3C, 0x80)
    pe[0x80:0x84] = b'PE\0\0'
    chars = 0x0102 | (0x2000 if f['name'].lower().endswith('.dll') else 0)
    if units[5][1] == 0: chars |= 0x0001  # relocs stripped
    struct.pack_into('<HHIIIHH', pe, 0x84, 0x01C2, nsec, ts, 0, 0, 224, chars)
    o = 0x98
    code_size = sum(len(s[2]) for s in sects if s[4] & 0x20)
    struct.pack_into('<HBBIIIIII', pe, o, 0x10B, 6, 0, code_size, 0, 0, entry, 0x1000, 0)
    struct.pack_into('<III', pe, o + 28, vbase, 0x1000, fa)
    struct.pack_into('<HHHHHHI', pe, o + 40, 4, 0, 0, 0, smaj, smin, 0)
    # SizeOfImage must cover every section; romimage's vsize leaves out .reloc, and the loader then
    # ignores the relocations (the DLL runs unrelocated)
    vsize = max([vsize] + [(s[1] + max(s[0], len(s[2])) + 0xFFF) & ~0xFFF for s in sects])
    struct.pack_into('<IIIHH', pe, o + 56, vsize, hsize, 0, subsys, 0)
    struct.pack_into('<IIIIII', pe, o + 72, stack, 0x1000, 0x100000, 0x1000, 0, 16)
    for i in range(min(ROM_EXTRA, 16)):
        struct.pack_into('<II', pe, o + 96 + i * 8, *units[i])
    sh = o + 224
    body = bytearray()
    for i, (ovs, rva, data, real, flags) in enumerate(sects):
        raw = data + b'\0' * ((-len(data)) % fa)
        ptr = hsize + len(body)
        struct.pack_into('<8sIIIIIIHHI', pe, sh + i * 40, secname(rva, flags), ovs, rva, len(raw), ptr if raw else 0, 0, 0, 0, 0, flags)
        body += raw
    info = {'vbase': rombase, 'fixups': nfix, 'stripped': units[5][1] == 0, 'datamoved': any(real and real != rombase + rva for (_, rva, _, real, fl) in sects if fl & 0x80000000)}
    return bytes(pe + body), info


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    img = Imgfs(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'list':
        print('compression', img.comp, 'files', len(img.files))
        for f in img.files:
            kind = 'M' if img.is_module(f) else 'F'
            total = f['size'] + sum(s['size'] for s in f['sects'])
            print('%s %9d %08x %s' % (kind, total, f['attr'], f['name']))
    elif cmd == 'get':
        out = sys.argv[3]; os.makedirs(out, exist_ok=True)
        pats = [p.lower() for p in sys.argv[4:]]
        for f in img.files:
            n = f['name'].lower()
            if not any(p == '*' or p == n or (p.startswith('~') and p[1:] in n) for p in pats):
                continue
            if img.is_module(f):
                data, info = rebuild_pe(img, f)
                print('M', f['name'], hex(info['vbase']), '%d fixups' % info['fixups'], 'datamoved' if info['datamoved'] else '')
            else:
                data = img.stream(f['size'], f['tbl'], f['tblsize'])
                print('F', f['name'], len(data))
            open(os.path.join(out, f['name']), 'wb').write(data)


if __name__ == '__main__':
    main()
