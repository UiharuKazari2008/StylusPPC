"""Builds HDRAW.dll, the DirectDraw caps shim for HTC games (guest/DdShim/hdraw.c), with
make-htcsensor.py's builder, as base64 into guest/Helper/HdrawDll.cs.

    python tools/make-hdraw.py [--out path]
"""
import importlib.util, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('make_htcsensor', os.path.join(ROOT, 'tools', 'make-htcsensor.py'))
mh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mh)

if __name__ == '__main__':
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    mh.build(os.path.join(ROOT, 'guest', 'DdShim', 'hdraw.c'), 'HDRAW.dll', ('DirectDraw',),
             os.path.join(ROOT, 'guest', 'Helper', 'HdrawDll.cs'), 'HdrawDll', out)
