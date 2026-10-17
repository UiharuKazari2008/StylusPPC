# Windows CE error-report dump (.kdmp, "CEDS") reader: exception, faulting thread registers, and the
# module each interesting address falls in.
#   python -I kdmp.py <file.kdmp>
# Layout (observed on WM 6.5): u32 'CEDS', u32 version, u32 stream count, u32 directory offset;
# directory entries (u32 type, u32 size, u32 rva). Table streams: u16 header size, u16 field-info
# size, u32 field count, u32 row count, u32 rows rva, then per field (u32 id, u32 size, u32 label
# rva, u32 format rva); rows are the fields packed back to back.
import sys, struct

S_EXC, S_MOD, S_THREADCTX, S_STACK = 0x8002, 0x8003, 0x8006, 0x8007


def main():
    d = open(sys.argv[1], 'rb').read()
    assert d[:4] == b'CEDS', 'not a CE dump'
    n, dirofs = struct.unpack_from('<II', d, 8)
    st = {}
    for i in range(n):
        t, sz, rva = struct.unpack_from('<III', d, dirofs + i * 12)
        st[t] = (rva, sz)

    def table(t):
        rva, _ = st[t]
        nf, cnt, data = struct.unpack_from('<III', d, rva + 4)
        sizes = [struct.unpack_from('<IIII', d, rva + 16 + k * 16)[1] for k in range(nf)]
        rows, o = [], data
        for _ in range(cnt):
            row = []
            for s in sizes:
                row.append(d[o:o + s]); o += s
            rows.append(row)
        return rows

    e = struct.unpack_from('<20I', d, st[S_EXC][0])
    pid, tid = e[2], e[3]
    code, addr = e[8], e[11]
    params = e[13:13 + e[12]] if e[12] < 8 else ()
    print('exception %08x at %08x, thread %08x, process %08x, info %s' % (code, addr, tid, pid, ' '.join('%08x' % p for p in params)))

    mods = []
    for r in table(S_MOD):
        name = r[0].split(b'\0')[0].decode('latin1')
        base, size = struct.unpack('<I', r[1])[0], struct.unpack('<I', r[2])[0]
        mods.append((base, size, name))

    def where(a):
        for base, size, name in mods:
            if base <= a < base + size:
                return '%s+%x' % (name, a - base)
        return ''

    for r in table(S_THREADCTX):
        if struct.unpack('<I', r[0])[0] != tid:
            continue
        ctx = r[1]
        regs = struct.unpack('<%dI' % (len(ctx) // 4), ctx)
        # Psr, R0..R12, Sp, Lr, Pc
        names = ['psr'] + ['r%d' % i for i in range(13)] + ['sp', 'lr', 'pc']
        for nm, v in zip(names, regs):
            w = where(v)
            print('  %-3s %08x %s' % (nm, v, w))
    print('pc module:', where(addr) or '?')


main()
