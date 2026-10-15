#!/usr/bin/env python3
# Kit H r5: kit H r4 with a profile's [environment] section and Wine patch 0958, for Z3CloudPlay's Environment variables and
# CPU cores settings. Two parts change:
#   - the any-title shell (eboot.bin), built on the Mac by make_shell.sh from the prospero-win commit that pins this
#     runtime: it reads [environment] and sets those variables for the game (capability settings-profile-environment);
#   - ntdll.prx, re-staged from build_cross.sh: Wine reads WINE_CPU_TOPOLOGY's processor count (patch 0958).
# Copy kit H r4's inputs (without titles/) and kit-spec.json to Z3CLOUDPLAY_KIT_DIR first (cp -c works), run make_shell.sh
# with Z3CLOUDPLAY_KIT_DIR set, then build_cross.sh in the VM, then this script in the VM with Z3CLOUDPLAY_RUNTIME_REVISION set to the
# runtime's prospero-win commit (it reads the cross build there; the Z3CloudPlay checkout is at the same path). Every other cross PRX
# must still match r4's. The template is unchanged.
import hashlib, json, os, shutil
from pathlib import Path

CAPABILITY = 'settings-profile-environment'
work = Path(os.environ.get('Z3CLOUDPLAY_KIT_WORK', Path.home() / 'z3cloudplay-kit'))
cross = work / 'wine-ps5/prx/sce_module'
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', 'build/kit-h-r5')).resolve()
previous = Path(os.environ.get('Z3CLOUDPLAY_KIT_FROM', 'build/kit-h-r4')).resolve()
runtime = here / 'inputs/runtime/wine'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

spec_path = here / 'kit-spec.json'
spec = json.loads(spec_path.read_text().replace(str(previous) + '/', str(here) + '/'))
assert CAPABILITY not in spec['capabilities'], 'kit already reads [environment]'

# ntdll.prx: the only cross PRX that changes.
drifted = [p.name for p in sorted(cross.glob('*.prx')) if sha(p) != sha(runtime / 'lib/wine/x86_64-unix' / p.name)]
assert drifted == ['ntdll.prx'], f'cross PRXs that differ from r4: {drifted}'
target = runtime / 'lib/wine/x86_64-unix/ntdll.prx'
mode = target.stat().st_mode & 0o7777
target.unlink()  # the inputs may be a copy-on-write clone of r4's
shutil.copyfile(cross / 'ntdll.prx', target); target.chmod(mode)
inventory = {f'wine/{p.relative_to(runtime)}': sha(p) for p in sorted(runtime.rglob('*')) if p.is_file()}
(here / 'inputs/runtime-inventory.json').write_text(json.dumps(inventory, indent=1, sort_keys=True) + '\n')

# The shell make_shell.sh built from the pinning commit, with r4's Mesa WGL/Zink providers.
variant = spec['variants'][0]
old_shell = Path(variant['path'].replace(str(here) + '/', str(previous) + '/'))
shells = [p for p in (here / 'inputs/titles').iterdir() if p.is_dir()]
assert len(shells) == 1, shells
shell = shells[0]
wrapper = json.loads((shell / 'native-title-build.json').read_text())['source_revision']
assert shell.name == f'any-{wrapper[:7]}', shell
assert sha(shell / 'lapy.elf') == sha(old_shell / 'lapy.elf'), 'the helper changed'
if not (shell / 'win').exists():
    shutil.copytree(old_shell / 'win', shell / 'win')
variant.update(id=f'any-title-{wrapper[:7]}', path=str(shell), wrapper_revision=wrapper,
               helper_manifest=str(shell / 'lapy-manifest.json'), native_build_manifest=str(shell / 'native-title-build.json'))
spec['sources']['wrapper']['revision'] = wrapper

revision = os.environ['Z3CLOUDPLAY_RUNTIME_REVISION']
assert len(revision) == 40, revision
artifact = f'{revision[:7]}-kit-h-r5'
spec['runtime'].update(wine_revision=revision, artifact_revision=artifact)
spec['sources']['wine']['revision'] = revision
spec['capabilities'] = sorted(set(spec['capabilities']) | {CAPABILITY})
spec_path.write_text(json.dumps(spec, indent=1, sort_keys=True) + '\n')

substitutions = json.loads((here / 'inputs/template-substitutions.json').read_text())
substitutions['runtime_artifact_revision'] = artifact
(here / 'inputs/template-substitutions.json').write_text(json.dumps(substitutions, indent=1) + '\n')
record = json.loads((here / 'inputs/stage-record.json').read_text())
record['r5'] = {'wine_patch': '0958-ntdll-honor-wine-cpu-topology-s-processor-count', 'changed': ['lib/wine/x86_64-unix/ntdll.prx'],
                'shell': shell.name, 'wrapper_revision': wrapper, 'capability': CAPABILITY}
record['prospero_win'] = revision
record['artifact_revision'] = artifact
(here / 'inputs/stage-record.json').write_text(json.dumps(record, indent=1) + '\n')
print('ntdll.prx', sha(target), 'shell', shell.name, 'files', len(inventory), artifact)
