#!/usr/bin/env python3
"""Check the independently pinned prospero source before compiling or packaging."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def check_source(source: Path, lock: Path, release: bool = False) -> str:
    pin = json.loads(lock.read_text())['prospero_win']
    revision = pin['revision']
    if not isinstance(revision, str) or not re.fullmatch(r'[0-9a-f]{40}', revision):
        raise ValueError('lock requires a full prospero revision')
    source = source.resolve(strict=True)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(source), *args],
                                       text=True, stderr=subprocess.PIPE).strip()
    if Path(git('rev-parse', '--show-toplevel')).resolve() != source:
        raise ValueError('prospero source must be a repository root')
    actual = git('rev-parse', 'HEAD')
    if actual != revision:
        raise ValueError(f'prospero revision mismatch: expected {revision}, found {actual}')
    if git('status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('prospero source is dirty; commit reviewed inputs before building')
    if release and pin.get('published') is not True:
        raise ValueError('prospero source pin is not published; release packaging is unavailable')
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'third_party/prospero-win')
    parser.add_argument('--lock', type=Path, default=ROOT / 'dependencies.lock.json')
    parser.add_argument('--release', action='store_true')
    args = parser.parse_args()
    try:
        print(check_source(args.source, args.lock, args.release))
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f'prospero source validation failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
