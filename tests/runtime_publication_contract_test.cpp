#include "runtime/publication_contract.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace z3cloudplay::runtime;

int main() {
  PwTitlePublication receipt{};
  receipt.schema_version = 1;
  receipt.phase = PW_TITLE_PUBLICATION_PENDING;
  std::strcpy(receipt.transaction_id, "00000000-0000-7000-8000-000000000001");
  std::strcpy(receipt.install_id, "install-220");
  std::strcpy(receipt.title_id, "FAKE78001");
  std::strcpy(receipt.kit_id, "kit-hl2");
  std::strcpy(receipt.runtime_bundle_id, "wine-wow64");
  std::strcpy(receipt.recipe_id, "steam-220-hl2-v1");
  std::strcpy(receipt.recipe_version, "1");
  std::memset(receipt.plan_sha256, 'a', 64);
  receipt.settings_revision = std::numeric_limits<std::uint64_t>::max();
  auto encoded_receipt = SerializePublicationReceipt(receipt);
  CHECK(encoded_receipt && encoded_receipt->find("18446744073709551615") != std::string::npos);
  PwTitlePublication read_receipt{};
  CHECK(pw_title_publication_parse(reinterpret_cast<const uint8_t*>(encoded_receipt->data()),
        encoded_receipt->size(), &read_receipt) == PW_OK);
  CHECK(read_receipt.settings_revision == receipt.settings_revision &&
        read_receipt.phase == PW_TITLE_PUBLICATION_PENDING);
  std::memset(receipt.publication_sha256, 'b', 64);
  CHECK(!SerializePublicationReceipt(receipt)); // Pending cannot claim committed digest.
  receipt.phase = PW_TITLE_PUBLICATION_COMMITTED;
  receipt.previous_revision = receipt.settings_revision;
  CHECK(SerializePublicationReceipt(receipt));
  std::memset(receipt.install_id, 'a', sizeof(receipt.install_id));
  CHECK(!SerializePublicationReceipt(receipt)); // Bounded even for invalid C callers.

  PwTitlePublicationMetadata metadata{};
  metadata.schema_version = 2;
  std::strcpy(metadata.install_id, "install-220");
  std::strcpy(metadata.title_id, "FAKE78001");
  std::strcpy(metadata.data_root, "/data/z3cloudplay/games/install-220");
  std::strcpy(metadata.profile_id, "z3cloudplay-steam-220");
  std::strcpy(metadata.runtime_bundle_id, "wine-wow64");
  std::strcpy(metadata.recipe_id, "steam-220-hl2-v1");
  std::strcpy(metadata.recipe_version, "1");
  std::strcpy(metadata.kit_id, "kit-hl2");
  metadata.settings_revision = std::numeric_limits<std::uint64_t>::max();
  for (auto field : {metadata.profile_sha256, metadata.descriptor_sha256, metadata.catalog_sha256})
    std::memset(field, 'a', 64);
  metadata.input_count = 1;
  std::strcpy(metadata.inputs[0].name, "gamepad.input");
  std::memset(metadata.inputs[0].sha256, 'b', 64);
  std::strcpy(metadata.dxvk_path, "/data/homebrew/FAKE78001/game/hl2/dxvk.conf");
  metadata.registry_count = 2;
  auto& version = metadata.registry[0];
  std::strcpy(version.key, "Software\\Wine\\AppDefaults\\hl2.exe");
  std::strcpy(version.name, "Version");
  version.kind = PW_TITLE_PUBLICATION_REGISTRY_STRING;
  std::strcpy(version.string_value, "win7");
  auto& dpi = metadata.registry[1];
  std::strcpy(dpi.key, "Control Panel\\Desktop");
  std::strcpy(dpi.name, "LogPixels");
  dpi.kind = PW_TITLE_PUBLICATION_REGISTRY_DWORD;
  dpi.dword = 120;
  auto encoded_metadata = SerializePublicationMetadata(metadata);
  CHECK(encoded_metadata);
  CHECK(encoded_metadata->find("registry.1.dword=00000078\n") != std::string::npos);
  CHECK(encoded_metadata->find("input.0.name_hex=67616d657061642e696e707574\n") != std::string::npos);
  PwTitlePublicationMetadata read_metadata{};
  CHECK(pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(encoded_metadata->data()),
        encoded_metadata->size(), &read_metadata) == PW_OK);
  CHECK(read_metadata.settings_revision == metadata.settings_revision);
  CHECK(std::strcmp(read_metadata.registry[0].key, version.key) == 0);
  CHECK(read_metadata.registry[1].dword == 120 && !read_metadata.dxvk_present);
  metadata.dxvk_present = 1;
  CHECK(!SerializePublicationMetadata(metadata)); // Present requires content hash.
  std::memset(metadata.dxvk_sha256, 'c', 64);
  CHECK(SerializePublicationMetadata(metadata));
  metadata.schema_version = 1;
  CHECK(!SerializePublicationMetadata(metadata));
  metadata.schema_version = 2;
  metadata.input_count = 65;
  CHECK(!SerializePublicationMetadata(metadata));
  metadata.input_count = 1;
  metadata.registry_count = 3;
  CHECK(!SerializePublicationMetadata(metadata));
  metadata.registry_count = 2;
  std::strcpy(metadata.recipe_id, "steam\nunknown=x");
  CHECK(!SerializePublicationMetadata(metadata));
  std::strcpy(metadata.recipe_id, "steam-220-hl2-v1");
  version.kind = PW_TITLE_PUBLICATION_REGISTRY_ABSENT;
  CHECK(!SerializePublicationMetadata(metadata)); // Refuse ignored semantic payload.
  version.string_value[0] = 0;
  CHECK(SerializePublicationMetadata(metadata));
  std::puts("publication contract: C++ producer validated by pinned C receipt/metadata parsers");
}
