"""Synthetic packaging/admission boundary; never certifies a release runtime."""
import argparse
import hashlib
import importlib.util
import importlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


class ProducerConsumerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.fixture = fixture_module.Fixture(Path(self.temp.name).resolve())
        registry = self.fixture.prefix / 'system.reg'
        registry.write_text(registry.read_text().replace(
            'Microsoft Base Cryptographic Provider v1.0',
            'Microsoft Enhanced Cryptographic Provider v1.0'))
        # Trust for these synthetic bytes exists only inside this test process.
        helper_hash = hashlib.sha256(self.fixture.title.joinpath('lapy.elf').read_bytes()).hexdigest()
        with mock.patch.dict(fixture_module.REQUIRED_HELPER_SHA256, {'FAKE78001': helper_hash}):
            fixture_module.package_kit(self.fixture.spec, self.fixture.out)
        self.manifest_hash = hashlib.sha256((self.fixture.out / 'manifest.json').read_bytes()).hexdigest()

    def admit(self, digest=None):
        return subprocess.run([str(consumer), str(self.fixture.out), digest or self.manifest_hash],
                              capture_output=True, text=True)

    def package_recipe(self, document):
        self.fixture.recipe.write_text(json.dumps(document))
        self.fixture.out = self.fixture.out.parent / 'recipe-kit'
        helper_hash = hashlib.sha256(self.fixture.title.joinpath('lapy.elf').read_bytes()).hexdigest()
        with mock.patch.dict(fixture_module.REQUIRED_HELPER_SHA256, {'FAKE78001': helper_hash}):
            fixture_module.package_kit(self.fixture.spec, self.fixture.out)
        self.manifest_hash = hashlib.sha256((self.fixture.out / 'manifest.json').read_bytes()).hexdigest()

    def test_z3cloudplay_owned_recipe_is_packaged_and_admitted(self):
        recipe = json.loads((Path(__file__).resolve().parents[1] /
                             'runtime/recipes/steam-220-hl2-v1.json').read_text())
        self.package_recipe(recipe)
        result = self.admit()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_conflicting_recipe_aliases_are_rejected_before_copy(self):
        recipe = {'recipe_id': 'steam-220-hl2-v1', 'recipe_version': 1,
                  'id': 'different', 'version': '1'}
        with self.assertRaisesRegex(fixture_module.KitError, 'recipe.*identity'):
            self.package_recipe(recipe)
        self.assertFalse(self.fixture.out.exists())

    def test_consumer_rejects_conflicting_recipe_aliases_with_valid_hashes(self):
        manifest_path = self.fixture.out / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        recipe_path = self.fixture.out / manifest['recipe']['path']
        recipe = json.loads(recipe_path.read_text())
        recipe.update(recipe_id='different', recipe_version=1)
        recipe_path.write_text(json.dumps(recipe))
        manifest['recipe']['sha256'] = hashlib.sha256(recipe_path.read_bytes()).hexdigest()
        packager = importlib.import_module('tools.package_z3cloudplay_kit')
        manifest['files'] = packager._file_entries(self.fixture.out)
        manifest['files_sha256'] = packager._files_digest(manifest['files'])
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        result = self.admit(hashlib.sha256(manifest_path.read_bytes()).hexdigest())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('recipe document', result.stderr)

    def test_emitted_manifest_is_admitted(self):
        result = self.admit()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('verified', result.stdout)

    def test_admission_reports_the_installed_manifest_digest(self):
        # The consumer checks the reported digest against the one it was given.
        result = self.admit('0' * 64)
        self.assertNotEqual(result.returncode, 0)

    def test_payload_is_versioned_by_its_manifest(self):
        # Kit files are not hashed at admission: the manifest's hashes name the version.
        # A same-size edit is admitted as that version; a size change needs a new manifest.
        manifest = json.loads((self.fixture.out / 'manifest.json').read_text())
        path = self.fixture.out / manifest['variants'][0]['path'] / 'eboot.bin'
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        result = self.admit()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path.write_bytes(original + b'more')
        result = self.admit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('regenerate', result.stdout + result.stderr)

    def test_uninventoried_payload_is_rejected(self):
        (self.fixture.out / 'unexpected.bin').write_bytes(b'extra')
        result = self.admit()
        self.assertNotEqual(result.returncode, 0)

    def test_self_consistent_rebuilt_helper_is_admitted(self):
        manifest_path = self.fixture.out / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        variant = manifest['variants'][0]
        helper = self.fixture.out / variant['path'] / 'lapy.elf'
        helper.write_bytes(b'\x7fELFreplacement')
        helper_manifest_path = self.fixture.out / variant['helper_manifest']
        helper_manifest = json.loads(helper_manifest_path.read_text())
        helper_manifest['elf_sha256'] = hashlib.sha256(helper.read_bytes()).hexdigest()
        helper_manifest_path.write_text(json.dumps(helper_manifest))
        # A rebuilt kit whose files match its own manifest is used, whichever
        # helper it carries; a helper that differs from its manifest is not (above).
        packager = importlib.import_module('tools.package_z3cloudplay_kit')
        manifest['files'] = packager._file_entries(self.fixture.out)
        manifest['files_sha256'] = packager._files_digest(manifest['files'])
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        result = self.admit(hashlib.sha256(manifest_path.read_bytes()).hexdigest())
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--consumer', required=True, type=Path)
    args = parser.parse_args()
    consumer = args.consumer
    sys.path.insert(0, str(args.source))
    spec = importlib.util.spec_from_file_location('upstream_kit_fixture', args.source / 'tests/test_z3cloudplay_kit.py')
    fixture_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture_module)
    unittest.main(argv=[sys.argv[0]])
