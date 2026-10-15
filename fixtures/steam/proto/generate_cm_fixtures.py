#!/usr/bin/env python3
"""Generate synthetic Steam CM fixtures with protoc from the pinned reference .proto files.

Every value is synthetic. The device identity mirrors the reference CMDeviceIdentity.swift
derivation (SHA-256 login ID, SHA-1 hex hardware keys) from a fixed synthetic seed, computed here
independently of the C++ implementation. Run from the repository root:
    python3 fixtures/steam/proto/generate_cm_fixtures.py
Set PROTOC to another protoc command (for example "python -m grpc_tools.protoc") and
PROTOC_INCLUDE to the directory holding google/protobuf/descriptor.proto when needed.
"""
import hashlib
import json
import os
import shlex
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[3]
PROTO = ROOT / "fixtures/steam/proto"
OUT = ROOT / "fixtures/steam/cm"
SEED = "6F1C2B8A-3D4E-4F50-9A6B-7C8D9E0F1A2B"


def device(seed):
    digest = hashlib.sha256(f"login-id:{seed}".encode()).digest()
    login_id = int.from_bytes(digest[:4], "big") or 1
    obj = b"\x00MessageObject\x00"
    for key in ("BB3", "FF2", "3B3"):
        obj += b"\x01" + key.encode() + b"\x00" + hashlib.sha1(f"{key}:{seed}".encode()).hexdigest().encode() + b"\x00"
    return login_id, obj + b"\x08\x08"


def octal(data):
    return "".join(f"\\{b:03o}" for b in data)


def package_buffer(package_id, apps):
    kv = b"\x00" + str(package_id).encode() + b"\x00"
    kv += b"\x02packageid\x00" + package_id.to_bytes(4, "little")
    kv += b"\x00appids\x00"
    for index, app in enumerate(apps):
        kv += b"\x02" + str(index).encode() + b"\x00" + app.to_bytes(4, "little")
    kv += b"\x08\x08\x08"  # ends appids, the package section and the root
    return (1).to_bytes(4, "little") + kv


login_id, machine_id = device(SEED)
FIXTURES = {
    "hello_request": ("steammessages_clientserver_login.proto", "CMsgClientHello", "protocol_version: 65580\n"),
    "logon_request": ("steammessages_clientserver_login.proto", "CMsgClientLogon", f"""protocol_version: 65580
cell_id: 0
client_package_version: 1771
client_language: "english"
client_os_type: 0
obfuscated_private_ip {{ v4: {login_id} }}
machine_id: "{octal(machine_id)}"
account_name: "synthetic_user"
machine_name: "Playden"
supports_rate_limit_response: true
access_token: "synthetic.refresh.token"
"""),
    "logon_response": ("steammessages_clientserver_login.proto", "CMsgClientLogonResponse",
                       "eresult: 1\nlegacy_out_of_game_heartbeat_seconds: 9\nheartbeat_seconds: 12\ncell_id: 7\n"),
    "license_list": ("steammessages_clientserver.proto", "CMsgClientLicenseList", """eresult: 1
licenses { package_id: 100 time_created: 1600000000 flags: 0 owner_id: 4242 access_token: 7001 }
licenses { package_id: 200 time_created: 1500000000 flags: 0 owner_id: 0 }
licenses { package_id: 300 time_created: 1400000000 flags: 8 owner_id: 4242 }
licenses { package_id: 400 time_created: 1300000000 flags: 0 owner_id: 9999 }
licenses { package_id: 500 time_created: 1200000000 minute_limit: 60 minutes_used: 60 owner_id: 4242 }
"""),
    "pics_request": ("steammessages_clientserver_appinfo.proto", "CMsgClientPICSProductInfoRequest",
                     "packages { packageid: 100 access_token: 7001 }\npackages { packageid: 200 access_token: 0 }\n"),
    "pics_response_part1": ("steammessages_clientserver_appinfo.proto", "CMsgClientPICSProductInfoResponse",
                            f'packages {{ packageid: 100 buffer: "{octal(package_buffer(100, [10, 20]))}" }}\nresponse_pending: true\n'),
    "pics_response_part2": ("steammessages_clientserver_appinfo.proto", "CMsgClientPICSProductInfoResponse",
                            f'packages {{ packageid: 200 buffer: "{octal(package_buffer(200, [20, 30]))}" }}\n'),
}

def encode(proto, message, text):
    command = shlex.split(os.environ.get("PROTOC", "protoc")) + [f"-I{PROTO}"]
    if os.environ.get("PROTOC_INCLUDE"):
        command.append(f"-I{os.environ['PROTOC_INCLUDE']}")
    return subprocess.run(command + [f"--encode={message}", str(PROTO / proto)],
                          input=text.encode(), capture_output=True, check=True).stdout


def frame(emsg, header_text, body):
    header = encode("steammessages_base.proto", "CMsgProtoBufHeader", header_text)
    return ((emsg | 0x80000000).to_bytes(4, "little") + len(header).to_bytes(4, "little") + header + body)


manifest = {"data_policy": "Synthetic values only; no real account, token, endpoint or license data.",
            "seed": SEED, "login_id": login_id, "machine_id_hex": machine_id.hex(),
            "generator": "fixtures/steam/proto/generate_cm_fixtures.py (protoc --encode)", "fixtures_sha256": {}}
for name, (proto, message, text) in FIXTURES.items():
    (OUT / f"{name}.textproto").write_text(text)
    encoded = encode(proto, message, text)
    (OUT / f"{name}.pb").write_bytes(encoded)
    manifest["fixtures_sha256"][f"{name}.pb"] = hashlib.sha256(encoded).hexdigest()
# Complete server frames for the client test. Account 4242 in the public universe; job 1 is the
# client's first job. The Multi body is gzip-compressed like real CM pushes.
SERVER = "steamid: 76561197960269970 client_sessionid: 77\n"
logon = frame(751, SERVER, (OUT / "logon_response.pb").read_bytes())
licenses = frame(780, SERVER, (OUT / "license_list.pb").read_bytes())
inner = b"".join(len(f).to_bytes(4, "little") + f for f in (logon, licenses))
import gzip as gzip_module
multi_body = encode("steammessages_base.proto", "CMsgMulti",
                    f'size_unzipped: {len(inner)}\nmessage_body: "{octal(gzip_module.compress(inner, mtime=0))}"\n')
server_frames = {
    "server_multi_logon.bin": frame(1, SERVER, multi_body),
    "server_pics_part1.bin": frame(8904, SERVER + "jobid_target: 1\n", (OUT / "pics_response_part1.pb").read_bytes()),
    "server_pics_part2.bin": frame(8904, SERVER + "jobid_target: 1\n", (OUT / "pics_response_part2.pb").read_bytes()),
}
for name, data in server_frames.items():
    (OUT / name).write_bytes(data)
    manifest["fixtures_sha256"][name] = hashlib.sha256(data).hexdigest()
(PROTO / "cm_fixture_manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
print("wrote", len(FIXTURES), "fixtures")
