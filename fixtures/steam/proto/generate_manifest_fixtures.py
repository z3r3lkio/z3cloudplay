#!/usr/bin/env python3
"""Generate synthetic Steam depot manifest fixtures with protoc from content_manifest.proto.

Every value is synthetic. The sectioned binary layout follows the pinned reference CDN.swift:
[magic u32 LE][length u32 LE][blob] for payload (0x71F617D0), metadata (0x1F4812BE) and signature
(0x1B81B817), then the end magic 0x32C415AB. The result is wrapped in a zip (stored and deflated).
Encrypted file names use Steam's symmetric scheme (AES-256-ECB-encrypted IV + AES-256-CBC/PKCS7)
with a fixed test key and IV, then base64. Run from the repository root:
    PROTOC="<venv>/bin/python -m grpc_tools.protoc" PROTOC_INCLUDE=<grpc_tools>/_proto \
        <venv>/bin/python fixtures/steam/proto/generate_manifest_fixtures.py
Requires the `cryptography` package for the AES step.
"""
import base64
import hashlib
import io
import json
import os
import pathlib
import shlex
import subprocess
import zipfile

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = pathlib.Path(__file__).resolve().parents[3]
PROTO = ROOT / "fixtures/steam/proto"
OUT = ROOT / "fixtures/steam/manifest"

PAYLOAD_MAGIC, METADATA_MAGIC, SIGNATURE_MAGIC, END_MAGIC = 0x71F617D0, 0x1F4812BE, 0x1B81B817, 0x32C415AB
DEPOT_ID, GID = 4242001, 7234567890123456789
KEY = bytes(range(0x10, 0x30))  # synthetic 32-byte depot key
IV = bytes(range(0xA0, 0xB0))  # fixed IV so the fixtures are reproducible


def octal(data):
    return "".join(f"\\{b:03o}" for b in data)


def sha1(label):
    return hashlib.sha1(label.encode()).digest()


def adler(data):
    a = b = 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return (b << 16) | a


# (path with a Windows separator, size, flags, link target, chunk sizes)
FILES = [
    ("bin\\game.exe", 3 * 1024 * 1024 + 123, 32, "", [1048576, 1048576, 1048576, 123]),
    ("data\\empty.dat", 0, 0, "", []),
    ("readme.txt", 1000, 0, "", [1000]),
]


def encrypt_name(name):
    padder = padding.PKCS7(128).padder()
    body = padder.update(name.encode()) + padder.finalize()
    enc = Cipher(algorithms.AES(KEY), modes.CBC(IV)).encryptor()
    iv_block = Cipher(algorithms.AES(KEY), modes.ECB()).encryptor()
    return iv_block.update(IV) + iv_block.finalize() + enc.update(body) + enc.finalize()


def payload_text(encrypted):
    lines = []
    for path, size, flags, link, chunks in FILES:
        shown = base64.b64encode(encrypt_name(path)).decode() if encrypted else path
        lines.append("mappings {")
        lines.append(f'  filename: "{octal(shown.encode())}"')
        lines.append(f"  size: {size}")
        lines.append(f"  flags: {flags}")
        lines.append(f'  sha_filename: "{octal(sha1("name:" + path))}"')
        if size:
            lines.append(f'  sha_content: "{octal(sha1("content:" + path))}"')
        offset = 0
        for index, chunk in enumerate(chunks):
            fake = f"{path}:{index}".encode()
            lines.append(f'  chunks {{ sha: "{octal(sha1(f"chunk:{path}:{index}"))}" crc: {adler(fake)} '
                         f"offset: {offset} cb_original: {chunk} cb_compressed: {chunk // 2 + 16} }}")
            offset += chunk
        lines.append("}")
    return "\n".join(lines) + "\n"


def protoc_encode(message, text):
    command = shlex.split(os.environ.get("PROTOC", "protoc")) + [f"-I{PROTO}"]
    if os.environ.get("PROTOC_INCLUDE"):
        command.append(f"-I{os.environ['PROTOC_INCLUDE']}")
    result = subprocess.run(command + [f"--encode={message}", str(PROTO / "content_manifest.proto")],
                            input=text.encode(), capture_output=True)
    if result.returncode != 0:
        raise SystemExit(result.stderr.decode())
    return result.stdout


def section(magic, blob):
    return magic.to_bytes(4, "little") + len(blob).to_bytes(4, "little") + blob


def manifest_binary(encrypted):
    payload = protoc_encode("ContentManifestPayload", payload_text(encrypted))
    total = sum(f[1] for f in FILES)
    metadata = protoc_encode("ContentManifestMetadata", f"""depot_id: {DEPOT_ID}
gid_manifest: {GID}
creation_time: 1700000000
filenames_encrypted: {"true" if encrypted else "false"}
cb_disk_original: {total}
cb_disk_compressed: {total // 2}
unique_chunks: 5
crc_encrypted: 305419896
crc_clear: 2271560481
""")
    signature = protoc_encode("ContentManifestSignature", f'signature: "{octal(bytes(range(64)))}"\n')
    return (section(PAYLOAD_MAGIC, payload) + section(METADATA_MAGIC, metadata) +
            section(SIGNATURE_MAGIC, signature) + END_MAGIC.to_bytes(4, "little"))


def make_zip(data, method):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        info = zipfile.ZipInfo("z", date_time=(2024, 1, 1, 0, 0, 0))
        info.compress_type = method
        archive.writestr(info, data)
    return buffer.getvalue()


OUT.mkdir(parents=True, exist_ok=True)
fixtures = {}
for label, encrypted in (("plain", False), ("encrypted", True)):
    binary = manifest_binary(encrypted)
    fixtures[f"manifest_{label}.bin"] = binary
    fixtures[f"manifest_{label}_stored.zip"] = make_zip(binary, zipfile.ZIP_STORED)
    fixtures[f"manifest_{label}_deflated.zip"] = make_zip(binary, zipfile.ZIP_DEFLATED)

# Lookup table for the C++ test's decrypt callback: base64 ciphertext <TAB> plaintext, one per line.
names = "".join(f"{base64.b64encode(encrypt_name(f[0])).decode()}\t{f[0]}\n" for f in FILES)
fixtures["encrypted_names.tsv"] = names.encode()

expected = [{"path": f[0].replace("\\", "/"), "size": f[1], "flags": f[2],
             "chunks": f[4]} for f in sorted(FILES, key=lambda f: f[0].replace("\\", "/"))]
index = {
    "data_policy": "Synthetic values only; no real depot, key or content data.",
    "generator": "fixtures/steam/proto/generate_manifest_fixtures.py (protoc --encode)",
    "depot_id": DEPOT_ID, "gid": GID, "key_hex": KEY.hex(), "iv_hex": IV.hex(),
    "expected_files": expected,
    "fixtures_sha256": {},
}
for name, data in fixtures.items():
    (OUT / name).write_bytes(data)
    index["fixtures_sha256"][name] = hashlib.sha256(data).hexdigest()
(OUT / "manifest_fixtures.json").write_text(json.dumps(index, indent=2, sort_keys=True) + "\n")
print("wrote", len(fixtures), "fixtures")
