# HTC R000FF NBH (HD2 RUU_signed.nbh) -> os.nb + partition dumps.
#   python -I nbh2nb.py <RUU_signed.nbh> <outdir>
# Container: 9-byte block headers (u32 len, u32 siglen, u8 flag) starting at 0x17, each followed by
# the data block and its signature. Inner image: HTCIMAGE header with a section table
# (types at 0x40, offsets at 0xC0, lengths at 0x140). The OS section (type 0x400) is NAND:
# 0x800-byte sectors, each followed by an 8-byte tag (u32 logical sector, u32 flags).
import sys, struct

src, out = sys.argv[1], sys.argv[2]
f = open(src, 'rb'); f.seek(0x17)
inner = bytearray()
while True:
    h = f.read(9)
    if len(h) < 9: break
    blen, slen, flag = struct.unpack('<IIB', h)
    if blen == 0: break
    inner += f.read(blen); f.read(slen)
types = struct.unpack('<32I', inner[0x40:0xC0])
offs = struct.unpack('<32I', inner[0xC0:0x140])
lens = struct.unpack('<32I', inner[0x140:0x1C0])
secs = [(t, o, l) for t, o, l in zip(types, offs, lens) if t]
print('sections', [(hex(t), hex(o), hex(l)) for t, o, l in secs])
for t, o, l in secs:
    if t != 0x400:
        open('%s/section_%x.bin' % (out, t), 'wb').write(inner[o:o + l])
        continue
    raw = inner[o:o + l]
    n = len(raw) // 0x808
    nb = bytearray(n * 0x800)
    for i in range(n):
        s = raw[i * 0x808:(i + 1) * 0x808]
        lsn = struct.unpack_from('<I', s, 0x800)[0]
        if lsn < n: nb[lsn * 0x800:(lsn + 1) * 0x800] = s[:0x800]
    open(out + '/os.nb', 'wb').write(nb)
    names = {0x20: 'xip_boot', 0x23: 'xip', 0x25: 'imgfs'}
    for i in range(4):
        e = nb[0x1BE + i * 16:0x1BE + i * 16 + 16]
        typ = e[4]; st, cnt = struct.unpack('<II', e[8:16])
        print('part', i, hex(typ), hex(st), hex(cnt))
        if typ in names:
            open('%s/p%d_%s.bin' % (out, i, names[typ]), 'wb').write(nb[st * 0x800:(st + cnt) * 0x800])
