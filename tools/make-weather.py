"""Builds SkyToday.dll, the classic Today screen weather item (guest/Weather/weather.c).

The shell calls InitializeCustomItem at ordinal 240. The DLL is stored as base64 in
guest/Helper/WeatherDll.cs and installed by guest/Helper/Weather.cs.

    python tools/make-weather.py [--out path]
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import importlib.util
spec = importlib.util.spec_from_file_location('make_htcsensor', os.path.join(ROOT, 'tools', 'make-htcsensor.py'))
mh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mh)

if __name__ == '__main__':
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    mh.build(os.path.join(ROOT, 'guest', 'Weather', 'weather.c'), 'SkyToday.dll', ('InitializeCustomItem',),
             os.path.join(ROOT, 'guest', 'Helper', 'WeatherDll.cs'), 'WeatherDll', out, ordinal_base=240)
