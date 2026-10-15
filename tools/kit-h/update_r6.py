#!/usr/bin/env python3
# Kit H r6: kit H r5 with the .NET Framework registry entries and files that Wine Mono's own installer writes (Gitea #75).
# Wine installs support/winemono-support.msi when mscoree registers in a prefix that finds Mono; kit H bottles never do,
# so mscoree's LoadLibraryShim had no HKLM\Software\Microsoft\.NETFramework InstallRoot (Antichamber). The template bottle
# gets what that MSI writes: its Registry table (.NETFramework InstallRoot and policy, NET Framework Setup\NDP, Active Setup,
# Post Platform, XNA) in the 64-bit and the Wow6432Node view, the CorSymWriter class that its fake diasymreader.dll registers,
# and C:\windows\Microsoft.NET (mscorlib/machine.config stubs from the MSI, Wine fake DLLs from dotnetfakedlls.inf). Windows
# Installer's record of the support product itself is left out (it names the scratch prefix's paths). Mono stays at the kit
# root. The runtime and the shell are r5's; a bottle made from an earlier template needs Recreate bottle.
#
# Z3CLOUDPLAY_DOTNET_DIR holds what one real install wrote, made on an x86_64 Linux host (Rosetta can't run wow64 Wine) with
# kit H's host Wine (11.17, .deps/wine-host/install of build_host.sh) and the kit's Mono support files:
#   WINEDLLOVERRIDES='mscoree,mshtml=' WINEPREFIX=a wineboot -u; cp -a a b
#   WINEDLLOVERRIDES='mscoree,mshtml=' WINEPREFIX=b wine msiexec /i Z:\...\wine-mono-11.3.0\support\winemono-support.msi /qn
#   base/system.reg = a/system.reg, installed/system.reg = b/system.reg, installed/Microsoft.NET = b/drive_c/windows/Microsoft.NET
#   msi/{Registry,File,MsiFileHash,Component}.idt = msidump of winemono-support.msi (msitools)
# The script takes only the keys the MSI's Registry table (or diasymreader) accounts for, fails on any other difference, and
# checks every file against the MSI's MD5 hashes or, for a fake DLL, the kit runtime's builtin of the same name.
# Copy kit H r5's inputs and kit-spec.json to Z3CLOUDPLAY_KIT_DIR first (cp -c works), then run on the Mac.
import hashlib, json, os, re, shutil, struct
from pathlib import Path

here = Path(os.environ.get('Z3CLOUDPLAY_KIT_DIR', 'build/kit-h-r6')).resolve()
previous = Path(os.environ.get('Z3CLOUDPLAY_KIT_FROM', 'build/kit-h-r5')).resolve()
dotnet = Path(os.environ.get('Z3CLOUDPLAY_DOTNET_DIR', here / 'dotnet')).resolve()
template = here / 'inputs/template/game'
runtime = here / 'inputs/runtime/wine'
support = here / 'inputs/mono/wine-mono-11.3.0/support'
PRODUCT_CODE = '{844FFFE4-801E-5614-B230-1A47C6191569}'  # Wine Mono Windows Support 11.3.0
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

spec_path = here / 'kit-spec.json'
spec = json.loads(spec_path.read_text().replace(str(previous) + '/', str(here) + '/'))
assert 'wine-mono' in spec['capabilities'], 'r6 starts from a kit with Wine Mono'

def idt(name):
    rows = [line.split('\t') for line in (dotnet / 'msi' / f'{name}.idt').read_text().split('\n')[3:] if line]
    return rows

# Registry: Wine's .reg text, a key per block ("[key] stamp", "#time=...", values, blank line).
def blocks(text):
    out, current = {}, None
    for line in text.split('\n'):
        if line.startswith('['):
            current = line[1:line.index('] ')] if '] ' in line else line[1:-1]
            out[current] = [line]
        elif current is not None:
            out[current].append(line)
    return {key: '\n'.join(lines).rstrip('\n') for key, lines in out.items()}
base = blocks((dotnet / 'base/system.reg').read_text())
installed = blocks((dotnet / 'installed/system.reg').read_text())
def values(block):
    joined = re.sub(r'\\\n *', '', block)  # a long value continues on the next line after a backslash
    return [line for line in joined.split('\n')[1:] if line and not line.startswith('#')]

# Every key the MSI's Registry table names, in the 64-bit view and the 32-bit (Wow6432Node) view.
def reg(path): return path.replace('\\', '\\\\')
allowed = set()
for row in idt('Registry'):
    key = row[2]
    assert row[1] == '2' and key.lower().startswith('software\\'), row
    allowed |= {reg(key).lower(), reg('Software\\Wow6432Node\\' + key[len('Software\\'):]).lower()}
# diasymreader.dll (a fake DLL from dotnetfakedlls.inf) registers its class when Wine installs it.
allowed |= {reg(k).lower() for k in ('Software\\Classes\\CLSID\\{0ae2deb0-f901-478b-bb9f-881ee8066788}',
                                     'Software\\Classes\\Wow6432Node\\CLSID\\{0ae2deb0-f901-478b-bb9f-881ee8066788}',
                                     'Software\\Classes\\CorSymWriter_SxS')}
# Windows Installer's own record of the support product (its cached package and the scratch prefix's source paths).
product = PRODUCT_CODE[1:-1].replace('-', '')
packed = (product[:8][::-1] + product[8:12][::-1] + product[12:16][::-1] +
          ''.join(product[i:i + 2][::-1] for i in range(16, 32, 2)))
upgrade = '{DE624609-C6B5-486A-9274-EF0B854F6BC5}'[1:-1].replace('-', '')
packed_upgrade = (upgrade[:8][::-1] + upgrade[8:12][::-1] + upgrade[12:16][::-1] +
                  ''.join(upgrade[i:i + 2][::-1] for i in range(16, 32, 2)))
skipped = [reg(k).lower() for k in (
    f'Software\\Classes\\Installer\\Products\\{packed}', f'Software\\Classes\\Installer\\Features\\{packed}',
    f'Software\\Classes\\Installer\\UpgradeCodes\\{packed_upgrade}',
    f'Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UpgradeCodes\\{packed_upgrade}',
    'Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Components',
    f'Software\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Products\\{packed}',
    f'Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{PRODUCT_CODE}',
    'Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunServices')]
# Differences a second Wine start makes on its own.
noise = [('system\\\\controlset001\\\\control\\\\session manager', '"PendingFileRenameOperations"='),
         ('system\\\\controlset001\\\\enum\\\\', '"ContainerId"=')]
gone = ['software\\\\microsoft\\\\windows nt\\\\currentversion\\\\networkcards']
def under(key, prefixes): return any(key == p or key.startswith(p + '\\\\') for p in prefixes)

added = {}
for key, block in installed.items():
    lowered = key.lower()
    if key in base:
        if values(block) != values(base[key]):
            changed = set(values(block)) ^ set(values(base[key]))
            assert all(any(lowered.startswith(k) and line.startswith(n) for k, n in noise) for line in changed), (key, changed)
        continue
    if under(lowered, skipped):
        continue
    assert under(lowered, allowed), f'key the MSI does not account for: {key}'
    added[key] = block
for key in base:
    # A key that only lost its place in the file because it gained a child.
    assert key in installed or key.lower() in gone or any(k.startswith(key + '\\\\') for k in installed), key
covered = {k.lower() for k in added}
for row in idt('Registry'):
    views = [reg(row[2]).lower(), reg('Software\\Wow6432Node\\' + row[2][len('Software\\'):]).lower()]
    assert any(under(k, [v]) for k in covered for v in views), f'Registry row not written: {row[0]}'
text = '\n'.join(added.values())
assert not re.search(r'scratch|/home/|\\\\home\\\\|leolobato|/Users/', text, re.I), 'a host path in the new keys'

def install_root(view):
    block = added[view]
    return re.search(r'^"InstallRoot"="(.*)"$', block, re.M).group(1)
roots = {'64-bit': install_root('Software\\\\Microsoft\\\\.NETFramework'),
         '32-bit': install_root('Software\\\\Wow6432Node\\\\Microsoft\\\\.NETFramework')}
assert roots == {'64-bit': 'C:\\\\windows\\\\Microsoft.NET\\\\Framework64\\\\',
                 '32-bit': 'C:\\\\windows\\\\Microsoft.NET\\\\Framework\\\\'}, roots

# Splice the keys into the template's system.reg in Wine's order (depth-first, names compared without case).
system_reg = template / 'system.reg'
lines = system_reg.read_text().split('\n')
existing = {line[1:line.index('] ')].lower() for line in lines if line.startswith('[') and '] ' in line}
assert not existing & covered, sorted(existing & covered)[:5]
def order(key): return [part.lower() for part in key.split('\\\\')]
for key, block in sorted(added.items(), key=lambda item: order(item[0])):
    index = next((i for i, line in enumerate(lines)
                  if line.startswith('[') and '] ' in line and order(line[1:line.index('] ')]) > order(key)), len(lines))
    lines[index:index] = block.split('\n') + ['']
system_reg.unlink()  # the inputs may be a copy-on-write clone of r5's
system_reg.write_text('\n'.join(lines))

# C:\windows\Microsoft.NET: the MSI's files (checked against its MD5 table) and the fake DLLs (the runtime's builtins).
msi_files = {row[0].replace('\\', '/'): row for row in idt('File')}
md5 = {row[0].replace('\\', '/'): struct.pack('<4i', *map(int, row[2:6])).hex() for row in idt('MsiFileHash')}
inf = (support / 'dotnetfakedlls.inf').read_text()
fakes = {}
for line in inf.split('\n'):
    match = re.match(r'^10,(Microsoft\.NET\\Framework(64)?\\[^,]+),(.+)$', line.strip())
    if match:
        fakes[f'{match.group(1)}\\{match.group(3)}'.replace('\\', '/')] = \
            runtime / 'lib/wine' / ('x86_64-windows' if match.group(2) else 'i386-windows') / match.group(3)
source = dotnet / 'installed/Microsoft.NET'
destination = template / 'drive_c/windows/Microsoft.NET'
assert not destination.exists(), destination
found = set()
for path in sorted(source.rglob('*')):
    relative = 'Microsoft.NET/' + path.relative_to(source).as_posix()
    if path.is_dir() or path.is_symlink():
        assert not path.is_symlink(), path
        continue
    found.add(relative)
    if relative in fakes:
        assert sha(path) == sha(fakes[relative]), f'{relative} is not the runtime builtin'
    else:
        assert relative in msi_files and hashlib.md5(path.read_bytes()).hexdigest() == md5[relative], relative
assert found == set(msi_files) | set(fakes), sorted((set(msi_files) | set(fakes)) ^ found)
shutil.copytree(source, destination)
for path in [destination, *destination.rglob('*')]:
    path.chmod(0o755 if path.is_dir() else 0o644)

record = json.loads((here / 'inputs/stage-record.json').read_text())
record['r6'] = {'wine_mono_support_msi': PRODUCT_CODE, 'msi_sha256': sha(support / 'winemono-support.msi'),
                'host_wine': 'wine-11.17 (kit H build_host.sh)', 'registry_keys': len(added),
                'install_root': {k: v.replace('\\\\', '\\') for k, v in roots.items()},
                'files': len(found), 'fake_dlls': len(fakes), 'template_changed': ['system.reg', 'drive_c/windows/Microsoft.NET']}
(here / 'inputs/stage-record.json').write_text(json.dumps(record, indent=1) + '\n')
spec_path.write_text(json.dumps(spec, indent=1, sort_keys=True) + '\n')
print('keys', len(added), 'files', len(found), 'fake DLLs', len(fakes), 'InstallRoot', record['r6']['install_root'])
