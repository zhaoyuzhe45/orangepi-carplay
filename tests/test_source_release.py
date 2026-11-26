"""Public-source boundary and local identity preparation regression tests."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SourceReleaseTests(unittest.TestCase):
    def run_script(self, name, *args):
        return subprocess.run([sys.executable, str(ROOT / 'scripts' / name), *map(str, args)],
                              capture_output=True, text=True)

    def test_current_public_file_set(self):
        result = self.run_script('check-public-source.py')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_force_added_private_key_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / '.gitignore').write_text('*.c\n')
            key = root / 'source.c'
            key.write_text('-----BEGIN PRIVATE KEY-----\n' + 'A' * 40 + '\n')
            subprocess.run(['git', '-C', str(root), 'add', '-f', 'source.c'], check=True)
            # Also detect an indexed secret after the working copy is sanitized.
            key.write_text('removed locally, still in index\n')
            result = self.run_script('check-public-source.py', '--root', root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('source.c', result.stdout + result.stderr)
            self.assertIn('index', result.stdout + result.stderr)
            self.assertNotIn('A' * 40, result.stdout + result.stderr)

    def test_ignored_local_key_is_allowed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / '.gitignore').write_text('*.pem\n')
            (root / 'private.pem').write_text('-----BEGIN PRIVATE KEY-----\nlocal\n')
            result = self.run_script('check-public-source.py', '--root', root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_wrong_firmware_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            library = Path(temp) / 'system/lib64/android.hardware.gnss@1.0.so'
            library.parent.mkdir(parents=True)
            library.write_bytes(b'not-the-supported-ROM')
            result = self.run_script('prepare-dependencies.py', '--firmware-root', temp)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('SHA256 mismatch', result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which('openssl'), 'OpenSSL required for identity tests')
    def test_generate_import_and_reject_mismatched_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            first, second = Path(temp) / 'first', Path(temp) / 'second'
            for root in (first, second):
                result = self.run_script('prepare-local-identity.py', '--root', root, '--generate')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((root / 'src/lockdown_identity.h').is_file())
                self.assertTrue((root / 'src/lockdown_tls_identity.h').is_file())
            before = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in first.rglob('*') if p.is_file()}
            result = self.run_script('prepare-local-identity.py', '--root', first, '--generate')
            self.assertNotEqual(result.returncode, 0, 'Must not silently replace an existing identity')
            result = self.run_script('prepare-local-identity.py', '--root', first)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(before, {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in before})
            shutil.copyfile(second / 'config/certs/lockdown-key.pem', first / 'config/certs/lockdown-key.pem')
            result = self.run_script('prepare-local-identity.py', '--root', first)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('match', result.stdout + result.stderr)
            for p in before:
                if p.parent.name == 'src':
                    self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(), before[p])


if __name__ == '__main__':
    unittest.main()
