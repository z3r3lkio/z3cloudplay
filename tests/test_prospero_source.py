import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SourcePinTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'Test')
        (self.source / 'tracked').write_text('original\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.revision = self.git('rev-parse', 'HEAD').strip()
        self.lock = self.root / 'lock.json'
        self.lock.write_text(json.dumps({'prospero_win': {
            'revision': self.revision, 'published': False}}))

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.source), *args], text=True)

    def check(self, *args):
        return subprocess.run([sys.executable, str(ROOT / 'tools/prospero_source.py'),
            '--lock', str(self.lock), '--source', str(self.source), *args],
            capture_output=True, text=True)

    def test_exact_clean_source_is_accepted(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(self.revision, result.stdout)

    def test_wrong_revision_is_rejected(self):
        (self.source / 'tracked').write_text('new\n')
        self.git('commit', '-qam', 'different')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('revision', result.stderr)

    def test_dirty_source_is_rejected(self):
        (self.source / 'tracked').write_text('modified\n')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('dirty', result.stderr)

    def test_untracked_source_is_rejected(self):
        (self.source / 'injected.c').write_text('unreviewed')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('dirty', result.stderr)

    def test_subdirectory_is_not_a_source_checkout(self):
        nested = self.source / 'nested'
        nested.mkdir()
        result = self.check('--source', str(nested))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('root', result.stderr)

    def test_unpublished_pin_cannot_be_released(self):
        result = self.check('--release')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('published', result.stderr)


class KitEntryTests(unittest.TestCase):
    def run_entry(self, *args):
        return subprocess.run(['bash', str(ROOT / 'scripts/build-prospero-kit.sh'), *args],
                              capture_output=True, text=True)

    def test_help_reaches_offline_packager(self):
        result = self.run_entry('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('package', result.stdout)

    def test_missing_arguments_show_usage(self):
        result = self.run_entry()
        self.assertEqual(result.returncode, 2)
        self.assertIn('Usage:', result.stderr)

    def test_release_rejects_unpublished_source_before_packaging(self):
        result = self.run_entry('--release', '--help')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not published', result.stderr)

    def test_package_requires_reviewed_preparation_before_copying(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            spec = root / 'spec.json'
            spec.write_text(json.dumps({'template': {'path': 'unprepared'}}))
            result = self.run_entry('package', '--spec', str(spec), '--out', str(root / 'kit'))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('preparation', result.stderr)
            self.assertFalse((root / 'kit').exists())


if __name__ == '__main__':
    unittest.main()
