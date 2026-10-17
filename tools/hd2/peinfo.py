# CE PE survey: imports + ISA level of code (ARMv5TE vs v6/v7/VFP/NEON) via a capstone linear sweep.
#   python -I peinfo.py <dir-containing-capstone> <out.json> <files...|@listfile>
# Data mixed into .text gives a few false hits; real v6/VFP code shows up as hundreds or thousands.
import sys, struct, json, os
sys.path.append(sys.argv[1])
import capstone

md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
md.skipdata = True

V6 = {'rev', 'rev16', 'revsh', 'uxtb', 'uxth', 'sxtb', 'sxth', 'uxtab', 'uxtah', 'sxtab', 'sxtah', 'uxtb16', 'sxtb16',
      'ldrex', 'strex', 'ldrexb', 'strexb', 'ldrexh', 'strexh', 'ldrexd', 'strexd', 'clrex', 'pkhbt', 'pkhtb', 'sel', 'ssat', 'usat',
      'ssat16', 'usat16', 'smlad', 'smladx', 'smlsd', 'smuad', 'smusd', 'smmul', 'smmla', 'smmls', 'umaal', 'usad8', 'usada8',
      'uadd8', 'usub8', 'sadd8', 'ssub8', 'uadd16', 'usub16', 'sadd16', 'ssub16', 'uqadd8', 'uqsub8', 'qadd8', 'qsub8', 'uhadd8',
      'cpsie', 'cpsid', 'setend', 'srsdb', 'rfeia', 'yield', 'wfi', 'wfe', 'sev'}
V7 = {'movw', 'movt', 'ubfx', 'sbfx', 'bfi', 'bfc', 'rbit', 'mls', 'dmb', 'dsb', 'isb', 'pldw', 'udiv', 'sdiv'}
CONDS = ('eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al')


def sections(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0': return None
    mach, nsec = struct.unpack_from('<HH', d, pe + 4)
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    opt = pe + 24
    base = struct.unpack_from('<I', d, opt + 28)[0]
    smaj, smin = struct.unpack_from('<HH', d, opt + 48)
    sub = struct.unpack_from('<H', d, opt + 68)[0]
    dirs = [struct.unpack_from('<II', d, opt + 96 + i * 8) for i in range(16)]
    secs = []
    for i in range(nsec):
        o = opt + optsz + i * 40
        vs, va, rs, rp = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, vs, rp, rs, struct.unpack_from('<I', d, o + 36)[0]))
    return dict(mach=mach, base=base, sub=sub, ver='%d.%d' % (smaj, smin), dirs=dirs, secs=secs)


def rva2off(info, rva):
    for va, vs, rp, rs, fl in info['secs']:
        if va <= rva < va + max(vs, rs):
            return rp + rva - va
    return None


def cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('latin1')


def imports(d, info):
    out = {}
    rva = info['dirs'][1][0]
    o = rva2off(info, rva) if rva else None
    while o is not None:
        ilt, ts, fc, name, iat = struct.unpack_from('<IIIII', d, o)
        if not name: break
        dll = cstr(d, rva2off(info, name))
        fns = []
        t = rva2off(info, ilt or iat)
        while t is not None:
            v = struct.unpack_from('<I', d, t)[0]
            if not v: break
            if v & 0x80000000: fns.append('#%d' % (v & 0xFFFF))
            else:
                p = rva2off(info, v)
                fns.append(cstr(d, p + 2) if p is not None else '?')
            t += 4
        out[dll] = fns
        o += 20
    return out


def isa(d, info):
    c = {'v6': 0, 'v7': 0, 'vfp': 0, 'neon': 0, 'insns': 0}
    for va, vs, rp, rs, fl in info['secs']:
        if not (fl & 0x20000000): continue
        for ins in md.disasm(d[rp:rp + min(vs, rs)], info['base'] + va):
            if ins.id == 0: continue
            c['insns'] += 1
            m = ins.mnemonic.split('.')[0]
            if len(m) > 4 and m.endswith(CONDS) and m[:-2] in V6 | V7:
                m = m[:-2]
            if m in V6: c['v6'] += 1
            elif m in V7: c['v7'] += 1
            elif m.startswith('v'):
                if 'q' in ins.op_str or any(t in ins.mnemonic for t in ('.i8', '.i16', '.i32', '.i64', '.u8', '.s8', '.u16', '.s16', '.8', '.16')):
                    c['neon'] += 1
                else:
                    c['vfp'] += 1
    return c


res = {}
paths = sys.argv[3:]
if len(paths) == 1 and paths[0].startswith('@'):
    paths = open(paths[0][1:], encoding='utf-8').read().splitlines()
for path in filter(None, paths):
    d = open(path, 'rb').read()
    if d[:2] != b'MZ': continue
    try:
        info = sections(d)
        if not info: continue
        r = dict(mach=hex(info['mach']), base=hex(info['base']), sub=info['sub'], ver=info['ver'],
                 relocs=bool(info['dirs'][5][0]), imports=imports(d, info))
        r.update(isa(d, info))
    except Exception as e:
        r = {'error': repr(e)}
    res[os.path.basename(path)] = r
json.dump(res, open(sys.argv[2], 'w'), indent=1)
