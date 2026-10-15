#!/usr/bin/env python3
# Kit H r3: kit H r2's inputs with kernelbase.dll (both architectures) re-staged from the rebuild with Wine patch 0957
# (GetComputerNameExW reads the stored computer name when wineboot's volatile ActiveComputerName key is missing).
# Copy kit H r2's inputs and kit-spec.json to Z3CLOUDPLAY_KIT_DIR first and set the spec's runtime revisions; run on the Linux
# build machine after build_cross.sh. The runtime copies and the template bottle's system32/syswow64 copies are replaced;
# every cross PRX must still match r2's. The inventory, template record and stage record are regenerated.
import hashlib, json, os, shutil
from pathlib import Path

MODULES = ('kernelbase.dll',)
work = Path(os.environ.get('Z3CLOUDPLAY_KIT_WORK', Path.home() / 'z3cloudplay-kit'))
cross = work / 'wine-ps5'
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', 'build/kit-h-r3')).resolve()
runtime = here / 'inputs/runtime/wine'
template = here / 'inputs/template/game'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def replace(source, target):
    mode = target.stat().st_mode & 0o7777
    target.unlink()  # the inputs may be a copy-on-write clone of r2's
    shutil.copyfile(source, target); target.chmod(mode)

drifted = [p.name for p in sorted((cross / 'prx/sce_module').glob('*.prx'))
           if sha(p) != sha(runtime / 'lib/wine/x86_64-unix' / p.name)]
assert not drifted, f'cross PRXs differ from r2: {drifted}'
changed = []
for arch in ('i386-windows', 'x86_64-windows'):
    for name in MODULES:
        source, target = cross / 'pe' / arch / name, runtime / 'lib/wine' / arch / name
        assert source.is_file() and target.is_file(), (source, target)
        assert sha(source) != sha(target), f'{arch}/{name} did not change'
        replace(source, target); changed.append(f'lib/wine/{arch}/{name}')
substitutions = json.loads((here / 'inputs/template-substitutions.json').read_text())
template_changed = []
for entry in substitutions['files']:
    if entry['runtime_source'] in changed:
        target = template / entry['template_path']
        assert sha(target) == entry['after_sha256'], entry['template_path']
        replace(runtime / entry['runtime_source'], target)
        entry['after_sha256'] = sha(target); template_changed.append(entry['template_path'])
assert len(template_changed) == len(changed), template_changed
spec = json.loads((here / 'kit-spec.json').read_text())
substitutions['runtime_artifact_revision'] = spec['runtime']['artifact_revision']
substitutions['changed'] = sum(f['before_sha256'] != f['after_sha256'] for f in substitutions['files'])
(here / 'inputs/template-substitutions.json').write_text(json.dumps(substitutions, indent=1) + '\n')
inventory = {f'wine/{p.relative_to(runtime)}': sha(p) for p in sorted(runtime.rglob('*')) if p.is_file()}
(here / 'inputs/runtime-inventory.json').write_text(json.dumps(inventory, indent=1, sort_keys=True) + '\n')
record = json.loads((here / 'inputs/stage-record.json').read_text())
record['r3'] = {'wine_patch': '0957-kernelbase-fall-back-to-the-stored-computer-name', 'changed': changed,
                'template_changed': template_changed}
record['cross_pe_modules'] = sorted(set(record['cross_pe_modules']) | {c.removeprefix('lib/wine/') for c in changed})
record['prospero_win'] = spec['runtime']['wine_revision']
record['artifact_revision'] = spec['runtime']['artifact_revision']
(here / 'inputs/stage-record.json').write_text(json.dumps(record, indent=1) + '\n')
print('changed', changed, 'template', template_changed, 'files', len(inventory), spec['runtime']['artifact_revision'])
