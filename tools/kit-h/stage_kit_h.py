#!/usr/bin/env python3
# Kit H inputs (prospero-win rebased onto upstream 644e32e; RADV from PS5_Vulkan 8aee3af / PS5_Mesa 01d7057; schannel via GnuTLS), run on the Linux VM with the Z3CloudPlay checkout shared from the Mac:
#   runtime  - kit F's runtime key set and modes, every file re-sourced from the kit G builds (prospero-win
#              feat/upstream-2026-10-10): x86_64-unix from the cross PRXs, the cross build's patched PE modules over the
#              host install, wowprospero from build_wowprospero.sh, wow64native from build_wow64native.sh, fonts from
#              the cross build; plus anything new the cross build links or the host install ships (kit D's rules).
#   template - kit A's template with kit A's 516 builtins re-sourced from this runtime, plus the template copies of every
#              patched PE module (system32/syswow64 and their drivers folders) and the five i386 HID drivers a 32-bit game
#              hosts (kit F's addition).
#   mesa     - upstream's Mesa WGL/Zink artifacts (tools/build_mesa_zink.sh) copied into the title shell's
#              win/mesa-zink/{i386,x86_64}-windows, where the title's wrapper installs them into an OpenGL game's prefix.
# Usage: stage_kit_h.py REVISION
# Z3CLOUDPLAY_KIT_WORK: the VM build folder (default ~/z3cloudplay-kit). Z3CLOUDPLAY_BUILD: the Z3CloudPlay build folder holding kits A and F
# (default build/ next to this script). Z3CLOUDPLAY_KIT_DIR: where kit H is staged (default build/kit-h). Z3CLOUDPLAY_MESA_ARTIFACTS:
# tools/build_mesa_zink.sh's artifacts (default ~/mesa-zink/work/artifacts).
import hashlib, json, os, shutil, subprocess, sys
from pathlib import Path

revision = sys.argv[1]
home = Path.home()
task = Path(os.environ.get('Z3CLOUDPLAY_KIT_WORK', home / 'z3cloudplay-kit'))
g = task / 'kit-h'
root = g / 'source'
install = root / '.deps/wine-host/install/usr'
cross = g / 'wine-ps5'
native_cpu = g / 'wow64native'
mesa = Path(os.environ.get('Z3CLOUDPLAY_MESA_ARTIFACTS', home / 'mesa-zink/work/artifacts'))
M = Path(os.environ.get('Z3CLOUDPLAY_BUILD', Path(__file__).resolve().parents[2] / 'build'))
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', M / 'kit-h'))
inputs = here / 'inputs'
out = inputs / 'runtime/wine'
template = inputs / 'template/game'
shell = inputs / 'titles' / f'any-{revision[:7]}'
assert not out.exists() and not template.exists(), 'inputs exist'
assert shell.is_dir(), shell
PACKAGE_RELEASE_SKIPS = {'winex11.drv', 'winegstreamer.dll', 'wpcap.dll', 'sane.ds', 'gphoto2.ds'}
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

# --- runtime
reference = M / 'kit-f/inputs/runtime/wine'
keys = {str(p.relative_to(reference)): p.stat().st_mode & 0o7777 for p in reference.rglob('*') if p.is_file()}
added = {}
for prx in sorted((cross / 'prx/sce_module').glob('*.prx')):
    rel = f'lib/wine/x86_64-unix/{prx.name}'
    if rel not in keys: keys[rel] = added[rel] = 0o755
for d in ('lib/wine/i386-windows', 'lib/wine/x86_64-windows', 'share/wine/nls'):
    for p in sorted((install / d).iterdir()):
        rel = f'{d}/{p.name}'
        if p.is_file() and rel not in keys and not p.name.endswith('.a') and p.name not in PACKAGE_RELEASE_SKIPS:
            keys[rel] = added[rel] = 0o644
ca = cross / 'prx/ca-certificates.crt'
if ca.is_file():
    keys['share/wine/ca-certificates.crt'] = added['share/wine/ca-certificates.crt'] = 0o644
missing, sources = [], {}
for rel, mode in sorted(keys.items()):
    parts = Path(rel).parts
    source = install / rel
    if rel == 'lib/wine/x86_64-windows/wow64native.dll':
        source = native_cpu / 'x86_64-windows/wow64native.dll'
    elif rel == 'lib/wine/x86_64-unix/wow64native.prx':
        source = native_cpu / 'ps5/wow64native.prx'
    elif parts[:3] == ('lib', 'wine', 'x86_64-unix'):
        source = cross / 'prx/sce_module' / parts[-1]
    elif parts[:3] in (('lib', 'wine', 'i386-windows'), ('lib', 'wine', 'x86_64-windows')):
        patched = cross / 'pe' / parts[2] / parts[-1]
        if patched.is_file(): source = patched
        if parts[-1] == 'wowprospero.dll':
            source = root / '.deps/wine-host/build/dlls/wowprospero/x86_64-windows/wowprospero.dll'
    elif rel == 'share/wine/ca-certificates.crt':
        source = ca
    elif parts[:3] == ('share', 'wine', 'fonts'):
        source = cross / 'prx/fonts' / parts[-1]
    if not source.is_file():
        missing.append(rel); continue
    sources[rel] = str(source)
    target = out / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target); target.chmod(mode)
assert not missing, missing
cross_pe = sorted(str(p.relative_to(cross / 'pe')) for p in (cross / 'pe').glob('*-windows/*') if p.is_file())
assert all(sources[f'lib/wine/{name}'].startswith(str(cross / 'pe')) for name in cross_pe), cross_pe
inventory = {f'wine/{p.relative_to(out)}': sha(p) for p in sorted(out.rglob('*')) if p.is_file()}
(inputs / 'runtime-inventory.json').write_text(json.dumps(inventory, indent=1, sort_keys=True) + '\n')

# --- template
kit_a = M / 'kit-a-6a77259/inputs'
subprocess.run(['cp', '-a', str(kit_a / 'template'), str(inputs / 'template')], check=True)
prior = json.loads((kit_a / 'template-substitutions.json').read_text())
assert prior['count'] == len(prior['files']) == 516
entries = [(e['template_path'], e['runtime_source'], e['after_sha256']) for e in prior['files']]
known = {e[0] for e in entries}
folders = {'x86_64-windows': 'drive_c/windows/system32', 'i386-windows': 'drive_c/windows/syswow64'}
template_added = []
for module in cross_pe:
    arch, name = module.split('/')
    for folder in (folders[arch], folders[arch] + '/drivers'):
        path = f'{folder}/{name}'
        if path not in known and (template / path).is_file():
            entries.append((path, f'lib/wine/{module}', sha(template / path))); known.add(path); template_added.append(path)
files = []
for path, runtime_source, expected_before in entries:
    target = template / path
    before = sha(target)
    assert before == expected_before, path
    mode = target.stat().st_mode & 0o7777
    shutil.copyfile(out / runtime_source, target); target.chmod(mode)
    files.append({'template_path': path, 'runtime_source': runtime_source, 'before_sha256': before, 'after_sha256': sha(target)})
drivers = template / 'drive_c/windows/syswow64/drivers'
drivers.mkdir(mode=(template / 'drive_c/windows/syswow64').stat().st_mode & 0o7777, exist_ok=True)
for name in ('hidclass.sys', 'hidparse.sys', 'winebus.sys', 'winehid.sys', 'winexinput.sys'):
    target = drivers / name
    if f'drive_c/windows/syswow64/drivers/{name}' in known: continue
    shutil.copyfile(out / 'lib/wine/i386-windows' / name, target); target.chmod(0o644)
    files.append({'template_path': str(target.relative_to(template)), 'runtime_source': f'lib/wine/i386-windows/{name}',
                  'before_sha256': None, 'after_sha256': sha(target)})
(inputs / 'template-substitutions.json').write_text(json.dumps(
    {'schema': 'z3cloudplay-template-substitution/1', 'base_preparation': 'hl2-fresh-v16-neutral-6a77259',
     'runtime_artifact_revision': f'{revision[:7]}-kit-h-r1', 'added_to_kit_a_set': template_added,
     'count': len(files), 'changed': sum(f['before_sha256'] != f['after_sha256'] for f in files), 'files': files},
    indent=1) + '\n')

# --- Mesa WGL/Zink in the title shell
manifest = json.loads((mesa / 'manifest.json').read_text())
zink = shell / 'win/mesa-zink'
assert not zink.exists()
for arch in ('i386-windows', 'x86_64-windows'):
    (zink / arch).mkdir(parents=True)
    for dll in sorted((mesa / arch).iterdir()):
        shutil.copyfile(dll, zink / arch / dll.name); (zink / arch / dll.name).chmod(0o644)
shutil.copyfile(mesa / 'manifest.json', shell / 'win/mesa-zink-manifest.json')

(inputs / 'stage-record.json').write_text(json.dumps(
    {'base': 'kit F runtime key set; kit A template', 'prospero_win': revision,
     'artifact_revision': f'{revision[:7]}-kit-h-r1', 'added_to_key_set': sorted(added), 'cross_pe_modules': cross_pe,
     'template_added': template_added, 'mesa_commit': manifest.get('mesa_commit'),
     'mesa_files': sorted(str(p.relative_to(shell)) for p in zink.rglob('*') if p.is_file())}, indent=1) + '\n')
print('runtime', len(inventory), 'files; added', sorted(added))
print('template', len(files), 'substitutions; added', template_added)
print('mesa', sorted(str(p.relative_to(shell)) for p in zink.rglob('*') if p.is_file()))
