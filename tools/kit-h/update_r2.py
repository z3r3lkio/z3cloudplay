#!/usr/bin/env python3
# Kit H r2: kit H's inputs with every cross PRX re-staged from the rebuild with Wine patch 0170 (ntdll records the path of
# a directory descriptor it receives from the in-process server, so names opened relative to a directory handle resolve).
# Copy kit H's inputs and kit-spec.json to Z3CLOUDPLAY_KIT_DIR first and set the spec's runtime revisions; run on the Linux
# build machine after build_cross.sh. Prints which runtime files changed; the inventory and stage record are regenerated.
import hashlib, json, os, shutil
from pathlib import Path

work = Path(os.environ.get('Z3CLOUDPLAY_KIT_WORK', Path.home() / 'z3cloudplay-kit'))
cross = work / 'wine-ps5/prx/sce_module'
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', 'build/kit-h-r2')).resolve()
runtime = here / 'inputs/runtime/wine'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
changed = []
for prx in sorted(cross.glob('*.prx')):
    target = runtime / 'lib/wine/x86_64-unix' / prx.name
    assert target.exists(), target
    if sha(prx) != sha(target):
        changed.append(prx.name)
        target.unlink()  # the inputs may be a copy-on-write clone of kit H's
        shutil.copyfile(prx, target); target.chmod(0o755)
inventory = {f'wine/{p.relative_to(runtime)}': sha(p) for p in sorted(runtime.rglob('*')) if p.is_file()}
(here / 'inputs/runtime-inventory.json').write_text(json.dumps(inventory, indent=1, sort_keys=True) + '\n')
spec = json.loads((here / 'kit-spec.json').read_text())
record = json.loads((here / 'inputs/stage-record.json').read_text())
record['r2'] = {'wine_patch': '0170-ntdll-ps5-track-directories-from-the-server', 'changed': changed}
record['prospero_win'] = spec['runtime']['wine_revision']
record['artifact_revision'] = spec['runtime']['artifact_revision']
(here / 'inputs/stage-record.json').write_text(json.dumps(record, indent=1) + '\n')
print('changed', changed, 'files', len(inventory), spec['runtime']['artifact_revision'])
