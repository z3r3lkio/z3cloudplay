#!/usr/bin/env python3
# Kit H r4: kit H r3 with Wine Mono 11.3.0 for .NET programs. Mono is kit-level: it is uploaded once with the kit, at
# mono/wine-mono-11.3.0, and is not copied into each title's runtime or each bottle. The template bottle's user.reg points
# Wine's mscoree at it (HKCU\Software\Wine\Mono RuntimePath = Z:\data\z3cloudplay\kits\<kit>\mono\wine-mono-11.3.0); a bottle
# made from an earlier template needs Recreate bottle. The runtime is unchanged (still 2b91e15-kit-h-r3).
# Copy kit H r3's inputs and kit-spec.json to Z3CLOUDPLAY_KIT_DIR first (cp -c works), then run on the Mac with
# Z3CLOUDPLAY_WINE_MONO_ARCHIVE set to https://dl.winehq.org/wine/wine-mono/11.3.0/wine-mono-11.3.0-x86.tar.xz.
# The archive's symbolic links are replaced by copies of their targets (kits carry no links) and the reference
# assemblies (lib/mono/*-api, not used at run time) are left out, with the two links into them.
import hashlib, json, os, shutil, sys, tarfile, tempfile
from pathlib import Path

VERSION = '11.3.0'
ARCHIVE_SHA256 = '54a1b0111c3fe4b785eae688af94d27e64995707ad648b6fee8000381b80d298'
URL = f'https://dl.winehq.org/wine/wine-mono/{VERSION}/wine-mono-{VERSION}-x86.tar.xz'
here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', 'build/kit-h-r4')).resolve()
previous = Path(os.environ.get('Z3CLOUDPLAY_KIT_FROM', 'build/kit-h-r3')).resolve()
archive = Path(os.environ['Z3CLOUDPLAY_WINE_MONO_ARCHIVE'])
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(archive) == ARCHIVE_SHA256, 'not the Wine Mono 11.3.0 archive'

spec_path = here / 'kit-spec.json'
spec_text = spec_path.read_text()
spec = json.loads(spec_text.replace(str(previous) + '/', str(here) + '/'))
kit_id = spec['kit_id']
assert 'wine-mono' not in spec['capabilities'], 'kit already has Wine Mono'

mono = here / f'inputs/mono/wine-mono-{VERSION}'
assert not mono.exists(), mono
with tempfile.TemporaryDirectory() as temporary:
    with tarfile.open(archive) as tar:
        tar.extractall(temporary, filter='data')
    source = Path(temporary).resolve() / f'wine-mono-{VERSION}'
    def reference(path): return any(part.endswith('-api') for part in path.relative_to(source).parts)
    copied = dropped = 0
    for path in sorted(source.rglob('*')):
        if reference(path) or path.is_dir():
            continue
        target = path.resolve()
        if source not in target.parents:
            sys.exit(f'link leaves the archive: {path}')
        if reference(target):
            dropped += 1
            continue
        destination = mono / path.relative_to(source)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(target, destination); destination.chmod(0o644)
        copied += 1
for directory in [mono, *mono.rglob('*')]:
    if directory.is_dir(): directory.chmod(0o755)

# The template's user.reg: Wine reads HKCU\Software\Wine\Mono RuntimePath, a path Wine opens through z: (/).
user_reg = here / 'inputs/template/game/user.reg'
registry = user_reg.read_text()
runtime_path = 'Z:' + f'/data/z3cloudplay/kits/{kit_id}/mono/wine-mono-{VERSION}'.replace('/', '\\')
key = 'Software\\\\Wine\\\\Mono'
assert f'[{key}]' not in registry
lines = registry.split('\n')
# Written like the template's Software\Wine\Drivers key, which also carries no timestamp.
section = [f'[{key}]', '"RuntimePath"="' + runtime_path.replace('\\', '\\\\') + '"', '']
# Wine keeps keys in order; put this one before the first later key.
index = next((i for i, line in enumerate(lines)
              if line.startswith('[') and line[1:].split(']')[0].lower() > key.lower()), len(lines))
lines[index:index] = section
user_reg.write_text('\n'.join(lines))

spec['capabilities'] = sorted(set(spec['capabilities']) | {'wine-mono'})
spec['mono'] = {'version': VERSION, 'path': str(mono)}
spec['sources']['mono'] = {'version': VERSION, 'url': URL, 'archive_sha256': ARCHIVE_SHA256}
spec_path.write_text(json.dumps(spec, indent=1, sort_keys=True) + '\n')
record = json.loads((here / 'inputs/stage-record.json').read_text())
record['r4'] = {'wine_mono': VERSION, 'archive_sha256': ARCHIVE_SHA256, 'files': copied,
                'dropped_reference_links': dropped, 'template_changed': ['user.reg'], 'runtime_path': runtime_path}
(here / 'inputs/stage-record.json').write_text(json.dumps(record, indent=1) + '\n')
print('mono files', copied, 'dropped links', dropped, 'RuntimePath', runtime_path)
