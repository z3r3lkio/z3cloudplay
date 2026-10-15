# Synthetic Steam authentication fixtures

These protobuf frames contain deterministic synthetic values only. They were encoded and decoded by the pinned SwiftProtobuf reference harness described in `../proto/auth_fixture_manifest.json`; the reduced C++ implementation cross-decodes every response and must reproduce every request byte-for-byte.

The `.textproto` files are readable descriptions of the matching `.pb` frames. They are not captured service traffic and contain no real account IDs, request IDs, tokens, QR challenges, guard data, or endpoints.
