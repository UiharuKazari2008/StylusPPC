# Copy one ROM package (by exact package-name prefix) plus extra files into a folder for the guest.
#   python -I stage.py <files-dir> <out-dir> <package-name-prefix> [extra-file ...]
# Registry from the package's .rgu is converted to UTF-8 <out>/<pkg>.reg for review.
import sys, os, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsm import read_dsm

root, out, prefix = sys.argv[1], sys.argv[2], sys.argv[3].lower()
os.makedirs(out, exist_ok=True)
hits = []
for fn in os.listdir(root):
    if fn.lower().endswith('.dsm'):
        pkg, files = read_dsm(os.path.join(root, fn))
        if pkg and pkg.lower().startswith(prefix):
            hits.append((pkg, files))
if len(hits) != 1:
    sys.exit('need exactly one package, got %s' % [h[0] for h in hits])
pkg, files = hits[0]
lower = {f.lower(): f for f in os.listdir(root)}
for f in files + sys.argv[4:]:
    src = lower.get(f.lower())
    if not src:
        print('missing', f); continue
    if f.lower().endswith('.rgu'):
        txt = open(os.path.join(root, src), 'rb').read().decode('utf-16')
        open(os.path.join(out, pkg + '.reg'), 'w', encoding='utf-8').write(txt)
        continue
    shutil.copyfile(os.path.join(root, src), os.path.join(out, src))
print(pkg, len(files), 'files ->', out)
