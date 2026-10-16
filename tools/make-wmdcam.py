"""Builds wmdcam.dll (WMDroid's frame generator for NULLCAM, see guest/Helper/wmdcam.s).

NDK clang assembles it and ld.lld links .text, .rdata and .data at IMAGEBASE + their RVAs; this
script wraps them in a minimal Windows CE ARM PE DLL with base relocations for the words listed in
.wmdrel (CE 5.2 places a DLL's writable data apart from its code, so code cannot reach .data
PC-relative) and writes guest/Helper/CameraDll.cs with the DLL as base64 for the helper to install.

    python tools/make-wmdcam.py [--out wmdcam.dll]
"""
import base64, glob, os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'guest', 'Helper', 'wmdcam.s')
CS = os.path.join(ROOT, 'guest', 'Helper', 'CameraDll.cs')

# name, RVA, PE section characteristics
SECTIONS = [
    ('.text', 0x1000, 0x60000020),   # code, execute, read
    ('.rdata', 0x2000, 0x40000040),  # initialized data, read
    ('.data', 0x3000, 0xC0000040),   # initialized data, read, write
]
IMAGEBASE = 0x10000000
LINKER_SCRIPT = 'SECTIONS { %s . = 0x%x; .wmdrel : { *(.wmdrel) } /DISCARD/ : { *(.ARM.attributes) } }' % (
    ' '.join('. = 0x%x; %s : { *(%s) }' % (IMAGEBASE + rva, name, name) for name, rva, _ in SECTIONS),
    IMAGEBASE + 0x8000)


def tools():
    sdk = os.environ.get('ANDROID_SDK') or os.path.join(os.environ['LOCALAPPDATA'], 'Android', 'Sdk')
    ndk = sorted(glob.glob(os.path.join(sdk, 'ndk', '*')))[-1]
    b = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin')
    return [os.path.join(b, n) for n in ('clang.exe', 'ld.lld.exe', 'llvm-objcopy.exe')]


def assemble():
    clang, lld, objcopy = tools()
    out = {}
    with tempfile.TemporaryDirectory() as d:
        obj, elf, lds = (os.path.join(d, 'wmdcam' + e) for e in ('.o', '.elf', '.lds'))
        open(lds, 'w').write(LINKER_SCRIPT)
        subprocess.check_call([clang, '--target=armv5te-none-eabi', '-c', SRC, '-o', obj])
        subprocess.check_call([lld, '-T', lds, '-e', '0', '-o', elf, obj])
        for name in [s[0] for s in SECTIONS] + ['.wmdrel']:
            raw = os.path.join(d, name[1:] + '.bin')
            subprocess.check_call([objcopy, '-O', 'binary', '--only-section=' + name, elf, raw])
            out[name] = open(raw, 'rb').read()
    return out


def align(v, a):
    return (v + a - 1) // a * a


def relocations(listing):
    """Base relocation blocks (IMAGE_REL_BASED_HIGHLOW) for the words whose addresses are listed."""
    rvas = sorted(v - IMAGEBASE for v in struct.unpack('<%dI' % (len(listing) // 4), listing))
    out = b''
    for page in sorted({r & ~0xFFF for r in rvas}):
        entries = [0x3000 | (r & 0xFFF) for r in rvas if r & ~0xFFF == page]
        if len(entries) % 2:
            entries.append(0)  # IMAGE_REL_BASED_ABSOLUTE padding
        out += struct.pack('<II%dH' % len(entries), page, 8 + 2 * len(entries), *entries)
    return out or struct.pack('<IIHH', 0x1000, 12, 0, 0)


def build_pe(sec):
    entry, exp, exp_n, imp, imp_n, iat, iat_n = struct.unpack_from('<7I', sec['.rdata'], 0)
    file_align, sect_align, headers = 0x200, 0x1000, 0x400
    reloc = relocations(sec.get('.wmdrel', b''))
    parts = [(name, rva, flags, sec[name]) for name, rva, flags in SECTIONS]
    reloc_rva = SECTIONS[-1][1] + align(len(sec['.data']), sect_align)
    parts.append(('.reloc', reloc_rva, 0x42000040, reloc))  # initialized data, discardable, read
    image_size = reloc_rva + sect_align

    shdrs, body, ptr = b'', b'', headers
    for name, rva, flags, data in parts:
        raw = align(len(data), file_align)
        shdrs += struct.pack('<8sIIIIIIHHI', name.encode(), len(data), rva, raw, ptr, 0, 0, 0, 0, flags)
        body += data.ljust(raw, b'\0')
        ptr += raw

    dos = bytearray(0x40)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x40)
    coff = struct.pack('<HHIIIHH', 0x01C0, len(parts), 0, 0, 0, 0xE0, 0x2102 | 0x000E)  # ARM, exec DLL, 32-bit
    dirs = [(0, 0)] * 16
    dirs[0] = (exp, exp_n)
    dirs[1] = (imp, imp_n)
    dirs[5] = (reloc_rva, len(reloc))
    dirs[12] = (iat, iat_n)
    code_size = align(len(sec['.text']), file_align)
    data_size = sum(align(len(p[3]), file_align) for p in parts[1:])
    opt = struct.pack('<HBBIIIIIII', 0x10B, 6, 0, code_size, data_size, 0, entry, 0x1000, 0x2000, IMAGEBASE)
    opt += struct.pack('<IIHHHHHHIIIIHHIIIIII', sect_align, file_align, 4, 20, 0, 0, 4, 20, 0, image_size, headers,
                       0, 9, 0, 0x10000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b''.join(struct.pack('<II', *d) for d in dirs)
    assert len(opt) == 0xE0
    hdr = bytes(dos) + b'PE\0\0' + coff + opt + shdrs
    assert len(hdr) <= headers
    return hdr.ljust(headers, b'\0') + body


def main():
    dll = build_pe(assemble())
    if '--out' in sys.argv:
        open(sys.argv[sys.argv.index('--out') + 1], 'wb').write(dll)
    b64 = base64.b64encode(dll).decode()
    lines = [b64[i:i + 100] for i in range(0, len(b64), 100)]
    body = ' +\n        '.join('"%s"' % l for l in lines)
    cs = ('// Generated by tools/make-wmdcam.py from wmdcam.s: do not edit.\n'
          'static class CameraDll\n{\n'
          '    // wmdcam.dll, %d bytes\n'
          '    public const string Base64 =\n        %s;\n}\n' % (len(dll), body))
    with open(CS, 'w', newline='\n') as f:
        f.write(cs)
    print('wmdcam.dll: %d bytes -> %s' % (len(dll), CS))


if __name__ == '__main__':
    main()
