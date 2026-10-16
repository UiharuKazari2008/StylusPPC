"""Scan a Windows CE ROM image (.nb0 flat or B000FF .bin) and print the XIP TOC."""
import struct, sys

def load(path):
    data = open(path, 'rb').read()
    if data[:7] == b'B000FF\n':
        start, length = struct.unpack_from('<II', data, 7)
        img = bytearray(length)
        off = 15
        while off < len(data):
            addr, ln, chk = struct.unpack_from('<III', data, off); off += 12
            if addr == 0:
                print(f'B000FF entry=0x{ln:08x}'); break
            img[addr - start:addr - start + ln] = data[off:off + ln]; off += ln
        return start, bytes(img)
    return None, data

def cstr(img, base, va, wide=False):
    o = va - base
    if o < 0 or o >= len(img): return f'<{va:08x}>'
    if wide:
        e = o
        while img[e:e+2] != b'\0\0': e += 2
        return img[o:e].decode('utf-16le', 'replace')
    return img[o:img.index(b'\0', o)].decode('latin1')

def main(path, at=0x40):
    start, img = load(path)
    i = img.find(b'ECEC', at)
    ptoc, = struct.unpack_from('<I', img, i + 4)
    tocoff, = struct.unpack_from('<I', img, i + 8)
    base = start if start is not None else ptoc - tocoff - (i - 0x40)
    print(f'{path}\n size=0x{len(img):x} ECEC@0x{i:x} pTOC=0x{ptoc:08x} base=0x{base:08x}')
    h = struct.unpack_from('<17I2H3I', img, ptoc - base)
    names = 'dllfirst dlllast physfirst physlast nummods RAMStart RAMFree RAMEnd copyEntries copyOffset profLen profOff numfiles kernelFlags fsRamPct drvGlobStart drvGlobLen cpu misc ext trackStart trackLen'.split()
    for n, v in zip(names, h): print(f'  {n:14s} 0x{v:08x}')
    nm, nf = h[4], h[12]
    o = ptoc - base + 0x54
    print(f' modules ({nm}):')
    if nm > 4096: return
    for k in range(nm):
        attr, t0, t1, size, name, e32, o32, ld = struct.unpack_from('<8I', img, o + 32 * k)
        print(f'  {cstr(img, base, name):32s} size=0x{size:06x} load=0x{ld:08x}')
    o += 32 * nm
    print(f' files ({nf}):')
    for k in range(nf):
        attr, t0, t1, rsz, csz, name, ld = struct.unpack_from('<7I', img, o + 28 * k)
        print(f'  {cstr(img, base, name):32s} size=0x{rsz:06x}')

if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x40)
