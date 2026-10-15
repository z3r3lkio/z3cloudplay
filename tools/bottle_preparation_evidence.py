#!/usr/bin/env python3
"""Verify a reviewed initialized export and its exact approved console template."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('source-binding.reviewed.json', 'loader-provenance.reviewed.json',
         'prefix.manifest', 'initialization-provenance.json')


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def entries(root):
    if root.is_symlink() or not root.is_dir():
        raise ValueError('prefix/template root is not an ordinary directory')
    result = {}
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            target = os.readlink(path)
            if (name, target) not in (('dosdevices/c:', '../drive_c'), ('dosdevices/z:', '/')):
                raise ValueError('prefix contains an unapproved symlink: ' + name)
            result[name] = {'type': 'symlink', 'sha256': hashlib.sha256(target.encode()).hexdigest(),
                            'target': target}
        elif stat.S_ISREG(info.st_mode):
            result[name] = {'type': 'file', 'sha256': digest(path)}
        elif not stat.S_ISDIR(info.st_mode):
            raise ValueError('prefix/template contains a special file: ' + name)
    return result


def template_digest(root):
    inventory = entries(root)
    if any(value['type'] != 'file' for value in inventory.values()):
        raise ValueError('console template contains symlinks instead of drive tables')
    for path in sorted(root.rglob('*')):
        name = path.relative_to(root).as_posix()
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            inventory[name] = {'type': 'directory'}
        inventory[name]['mode'] = f'{stat.S_IMODE(info.st_mode):04o}'
        if stat.S_ISREG(info.st_mode):
            inventory[name]['size'] = info.st_size
    return hashlib.sha256(json.dumps(inventory, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def verify(export, template, pin):
    metadata = export / 'metadata'
    for name in NAMES:
        path = metadata / name
        if path.is_symlink() or not path.is_file() or digest(path) != pin['files'][name]:
            raise ValueError('preparation evidence differs from independent pin: ' + name)
    binding = json.loads((metadata / NAMES[0]).read_text())
    loader = json.loads((metadata / NAMES[1]).read_text())
    init = json.loads((metadata / NAMES[3]).read_text())
    for document, schema in ((binding, 'prospero-win-wine-source-binding/1'),
                             (loader, 'prospero-win-z3cloudplay-loader-provenance/1'),
                             (init, 'prospero-win-z3cloudplay-bottle-initialization/1')):
        if document.get('schema') != schema:
            raise ValueError('unsupported preparation schema')
    for field in ('wine_source_head', 'retained_runtime_commit'):
        if binding.get(field) != pin[field] or loader.get(field) != pin[field]:
            raise ValueError('preparation source binding revision mismatch: ' + field)
    for field, value in binding.items():
        if field != 'schema' and loader.get(field) != value:
            raise ValueError('loader differs from reviewed source binding: ' + field)
    for document in (loader, init):
        if document.get('source_binding_sha256') != pin['files'][NAMES[0]]:
            raise ValueError('preparation source binding digest mismatch')
    if init.get('loader_provenance_sha256') != pin['files'][NAMES[1]] or \
            init.get('prefix_manifest_sha256') != pin['files'][NAMES[2]] or init.get('user') != 'prospero':
        raise ValueError('initialization provenance chain/user mismatch')
    inventory = entries(export / 'bottle')
    lines = []
    # Match preparation's component-wise pathlib ordering, rather than sorting
    # full strings ("Program Files" and "Program Files (x86)" differ there).
    for name, value in inventory.items():
        line = value['type'] + '\t' + value['sha256'] + '\t' + name
        if value['type'] == 'symlink':
            line += '\t' + value['target']
        lines.append(line + '\n')
    if ''.join(lines) != (metadata / 'prefix.manifest').read_text():
        raise ValueError('initialized prefix differs from reviewed manifest')
    if template_digest(template) != pin['template_inventory_sha256']:
        raise ValueError('console template differs from independent approved inventory')
    return len(inventory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', required=True, type=Path)
    parser.add_argument('--template', required=True, type=Path)
    parser.add_argument('--id', required=True)
    parser.add_argument('--lock', type=Path, default=ROOT / 'dependencies.lock.json')
    args = parser.parse_args()
    try:
        pin = json.loads(args.lock.read_text())['prospero_bottle_preparation'][args.id]
        count = verify(args.export, args.template, pin)
        print(f'verified {count} initialized entries and approved console template; native acceptance remains required')
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'bottle preparation rejected: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
