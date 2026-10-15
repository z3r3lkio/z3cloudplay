#!/usr/bin/env python3
# Kit H r8: kit H r6 with ntdll.prx rebuilt from the prospero-win commit that adds Wine patch 0603 (upstream PR #688): Windows
# memory queries report the PS5 title's direct-memory budget and its largest free block instead of the sandbox's empty BSD
# readings, so a game that checks free memory before it starts (Fallout: New Vegas) sees about 12 GiB and an equal page file.
# Only ntdll.prx changes; the shell, the template and Mono are r6's, so bottles need no Recreate. (r7 was a development kit.)
# Copy kit H r6's inputs and kit-spec.json to Z3CLOUDPLAY_KIT_DIR first (cp -c works). Build the cross PRXs with
# tools/build_wine_ps5.sh on the runtime's prospero-win commit, with build_cross.sh's arguments and the same work folder
# (wine-ps5; move r6's aside): assert messages keep the source path, so another folder name changes every PRX. Then run this
# script with Z3CLOUDPLAY_CROSS_PRX at that build's prx/sce_module (copied to the Mac or read in the VM) and
# Z3CLOUDPLAY_RUNTIME_REVISION set to that commit. Every other cross PRX must still match r6's.
import hashlib, json, os, shutil
from pathlib import Path

work = Path(os.environ.get('Z3CLOUDPLAY_KIT_WORK', Path.home() / 'z3cloudplay-kit'))
cross = Path(os.environ.get('Z3CLOUDPLAY_CROSS_PRX', work / 'wine-ps5/prx/sce_module')).resolve()
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', 'build/kit-h-r8')).resolve()
previous = Path(os.environ.get('Z3CLOUDPLAY_KIT_FROM', 'build/kit-h-r6')).resolve()
runtime = here / 'inputs/runtime/wine'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

spec_path = here / 'kit-spec.json'
spec = json.loads(spec_path.read_text().replace(str(previous) + '/', str(here) + '/'))
assert spec['runtime']['artifact_revision'].endswith('-kit-h-r5'), 'r8 starts from r6, which kept r5\'s runtime'

# ntdll.prx: the only cross PRX that changes.
drifted = [p.name for p in sorted(cross.glob('*.prx')) if sha(p) != sha(runtime / 'lib/wine/x86_64-unix' / p.name)]
assert drifted == ['ntdll.prx'], f'cross PRXs that differ from r6: {drifted}'
target = runtime / 'lib/wine/x86_64-unix/ntdll.prx'
mode = target.stat().st_mode & 0o7777
target.unlink()  # the inputs may be a copy-on-write clone of r6's
shutil.copyfile(cross / 'ntdll.prx', target); target.chmod(mode)
inventory = {f'wine/{p.relative_to(runtime)}': sha(p) for p in sorted(runtime.rglob('*')) if p.is_file()}
(here / 'inputs/runtime-inventory.json').write_text(json.dumps(inventory, indent=1, sort_keys=True) + '\n')

revision = os.environ['Z3CLOUDPLAY_RUNTIME_REVISION']
assert len(revision) == 40, revision
artifact = f'{revision[:7]}-kit-h-r8'
spec['runtime'].update(wine_revision=revision, artifact_revision=artifact)
spec['sources']['wine']['revision'] = revision
spec_path.write_text(json.dumps(spec, indent=1, sort_keys=True) + '\n')

substitutions = json.loads((here / 'inputs/template-substitutions.json').read_text())
substitutions['runtime_artifact_revision'] = artifact
(here / 'inputs/template-substitutions.json').write_text(json.dumps(substitutions, indent=1) + '\n')
record = json.loads((here / 'inputs/stage-record.json').read_text())
record['r8'] = {'wine_patch': '0603-ntdll-report-the-ps5-title-s-memory', 'changed': ['lib/wine/x86_64-unix/ntdll.prx']}
record['prospero_win'] = revision
record['artifact_revision'] = artifact
(here / 'inputs/stage-record.json').write_text(json.dumps(record, indent=1) + '\n')
print('ntdll.prx', sha(target), 'files', len(inventory), artifact)
