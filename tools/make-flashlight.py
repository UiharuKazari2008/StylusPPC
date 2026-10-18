"""Builds HTCFlashlightLib.dll, PocketStylus' LED for HTC Flashlight (guest/Flash/flashlight.c).

NDK clang compiles the C file freestanding for ARMv4T; make-htcsensor.py's builder links it into a
Windows CE ARM DLL. The DLL goes as base64 into guest/Helper/FlashDll.cs, and the helper installs
it as \\Windows\\HTCFlashlightLib.dll.

    python tools/make-flashlight.py [--out path]
"""
import importlib.util, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, 'tools', file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hs = load('make_htcsensor', 'make-htcsensor.py')


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    hs.build(os.path.join(ROOT, 'guest', 'Flash', 'flashlight.c'), 'HTCFlashlightLib.dll',
             ('EnableFlashLight', 'SetFlashLightDegree'),
             os.path.join(ROOT, 'guest', 'Helper', 'FlashDll.cs'), 'FlashDll', out)


if __name__ == '__main__':
    main()
