import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.export = self.root / 'export'
        self.bottle = self.export / 'bottle'
        self.meta = self.export / 'metadata'
        self.bottle.mkdir(parents=True)
        self.meta.mkdir()
        (self.bottle / 'system.reg').write_text('initialized\n')
        self.template = self.root / 'template'
        self.template.mkdir()
        (self.template / 'system.reg').write_text('console\n')
        self.binding = {'schema': 'prospero-win-wine-source-binding/1',
                        'retained_runtime_commit': 'e' * 40, 'wine_source_head': 'a' * 40,
                        'wine_source_diff_sha256': 'b' * 64}
        self.write('source-binding.reviewed.json', self.binding)
        loader = {'schema': 'prospero-win-z3cloudplay-loader-provenance/1', **self.binding,
                  'source_binding_sha256': self.hash('source-binding.reviewed.json')}
        loader['schema'] = 'prospero-win-z3cloudplay-loader-provenance/1'
        self.write('loader-provenance.reviewed.json', loader)
        (self.meta / 'prefix.manifest').write_text(
            'file\t' + hashlib.sha256(b'initialized\n').hexdigest() + '\tsystem.reg\n')
        self.init = {'schema': 'prospero-win-z3cloudplay-bottle-initialization/1', 'user': 'prospero',
                     'source_binding_sha256': self.hash('source-binding.reviewed.json'),
                     'loader_provenance_sha256': self.hash('loader-provenance.reviewed.json'),
                     'prefix_manifest_sha256': self.hash('prefix.manifest')}
        self.write('initialization-provenance.json', self.init)
        self.pin = {'files': {name: self.hash(name) for name in (
            'source-binding.reviewed.json', 'loader-provenance.reviewed.json',
            'prefix.manifest', 'initialization-provenance.json')},
            'wine_source_head': 'a' * 40, 'retained_runtime_commit': 'e' * 40,
            'template_inventory_sha256': evidence.template_digest(self.template)}

    def write(self, name, document):
        (self.meta / name).write_text(json.dumps(document, sort_keys=True) + '\n')

    def hash(self, name):
        return hashlib.sha256((self.meta / name).read_bytes()).hexdigest()

    def test_bound_initialization_and_template_pass(self):
        evidence.verify(self.export, self.template, self.pin)

    def test_initial_prefix_drift_is_rejected(self):
        (self.bottle / 'system.reg').write_text('changed\n')
        with self.assertRaisesRegex(ValueError, 'prefix'):
            evidence.verify(self.export, self.template, self.pin)

    def test_console_template_drift_is_rejected(self):
        (self.template / 'system.reg').write_text('substitute\n')
        with self.assertRaisesRegex(ValueError, 'template'):
            evidence.verify(self.export, self.template, self.pin)

    def test_template_permission_drift_is_rejected(self):
        path = self.template / 'system.reg'
        path.chmod(0o777)
        with self.assertRaisesRegex(ValueError, 'template'):
            evidence.verify(self.export, self.template, self.pin)

    def test_template_empty_directory_change_is_rejected(self):
        (self.template / 'unexpected').mkdir()
        with self.assertRaisesRegex(ValueError, 'template'):
            evidence.verify(self.export, self.template, self.pin)

    def test_evidence_cannot_supply_its_own_pin(self):
        self.init['user'] = 'other'
        self.write('initialization-provenance.json', self.init)
        with self.assertRaisesRegex(ValueError, 'pin'):
            evidence.verify(self.export, self.template, self.pin)

    def test_inconsistent_chain_is_rejected_even_with_new_document_pin(self):
        self.init['source_binding_sha256'] = '0' * 64
        self.write('initialization-provenance.json', self.init)
        self.pin['files']['initialization-provenance.json'] = self.hash('initialization-provenance.json')
        with self.assertRaisesRegex(ValueError, 'binding'):
            evidence.verify(self.export, self.template, self.pin)


spec = importlib.util.spec_from_file_location('evidence', ROOT / 'tools/bottle_preparation_evidence.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)

if __name__ == '__main__':
    unittest.main()
