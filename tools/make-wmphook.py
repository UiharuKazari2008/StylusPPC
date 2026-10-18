"""Builds WmpHook.dll, the in-process Windows Media Player plug-in (guest/WmpHook/wmp.c).

Same PE path as MAPIlib (tools/make-mapilib.py): NDK clang for ARMv4T, then a Windows CE ARM DLL.

    python tools/make-wmphook.py [--out path]   (default: app/src/main/assets/guest/WmpHook.dll)
"""
import importlib.util, os, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('make_mapilib', os.path.join(ROOT, 'tools', 'make-mapilib.py'))
mm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mm)

OUT = os.path.join(ROOT, 'app', 'src', 'main', 'assets', 'guest', 'WmpHook.dll')
SRC = os.path.join(ROOT, 'guest', 'WmpHook', 'wmp.c')


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else OUT
    with tempfile.TemporaryDirectory() as d:
        elf = mm.build_elf(d, [(SRC, False)])
    entry, sections, symbols = mm.ma.parse_elf(elf)
    dll, exports, dlls, nrel = mm.build_pe(entry, sections, symbols, mm.abs32_relocs(elf, sections),
                                           dllname='WmpHook.dll', prefixes=('Dll',))
    with open(out, 'wb') as f:
        f.write(dll)
    print('WmpHook.dll: %d bytes, exports %s, imports from %s, %d relocations -> %s' % (
        len(dll), ', '.join(exports), ', '.join(dlls), nrel, out))


if __name__ == '__main__':
    main()
