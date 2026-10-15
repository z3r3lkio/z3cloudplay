#!/usr/bin/env python3
"""Bind packaging to Z3CloudPlay's reviewed bottle evidence before copying assets."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from bottle_preparation_evidence import verify
from prospero_source import check_source

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--lock', type=Path, default=ROOT / 'dependencies.lock.json')
    parser.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        check_source(args.source, args.lock)
        if args.arguments and args.arguments[0] == 'package' and '--help' not in args.arguments:
            package = argparse.ArgumentParser()
            package.add_argument('--spec', required=True, type=Path)
            package.add_argument('--out', required=True, type=Path)
            inputs, _ = package.parse_known_args(args.arguments[1:])
            spec = json.loads(inputs.spec.read_text())
            template = spec['template']
            preparation = template['preparation']
            pin = json.loads(args.lock.read_text())['prospero_bottle_preparation'][preparation['id']]
            def input_path(value):
                path = Path(value)
                return path if path.is_absolute() else inputs.spec.resolve().parent / path
            verify(input_path(preparation['export']), input_path(template['path']), pin)
        return subprocess.run([sys.executable, str(args.source / 'tools/package_z3cloudplay_kit.py'),
                               *args.arguments]).returncode
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f'kit packaging rejected: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
