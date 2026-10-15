import hashlib
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativePackageIdentityTests(unittest.TestCase):
    def test_package_identity_binds_title_and_signed_binary(self):
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name)
            image = b"signed-title-fixture"
            (app / "eboot.bin").write_bytes(image)
            result = subprocess.run([
                "cmake", f"-DAPP_DIR={app}", "-DTITLE_ID=FAKE77031", "-P",
                str(ROOT / "cmake/WriteNativePackageIdentity.cmake")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((app / "z3cloudplay-package.identity").read_text(),
                             "z3cloudplay-native/1\nFAKE77031\n" + hashlib.sha256(image).hexdigest() + "\n")
            self.assertTrue((app / "logs").is_dir())
            self.assertTrue((app / "z3cloudplay-data").is_dir())

    def test_missing_binary_or_invalid_title_does_not_publish_identity(self):
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name)
            for title in ("FAKE77031", "../FAKE77031"):
                if title.startswith("../"):
                    (app / "eboot.bin").write_bytes(b"signed-title-fixture")
                result = subprocess.run([
                    "cmake", f"-DAPP_DIR={app}", f"-DTITLE_ID={title}", "-P",
                    str(ROOT / "cmake/WriteNativePackageIdentity.cmake")],
                    capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((app / "z3cloudplay-package.identity").exists())


if __name__ == "__main__":
    unittest.main()
