#pragma once

#include "runtime/game_install.h"
#include "runtime/package_installer.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// What this Playden build accepts. A kit is admitted when it is complete and its files
// have the sizes and modes its own manifest lists, whichever prospero-win build it comes
// from; what it was built from is reported, not compared (Settings > About shows it).
// Kit files are not hashed: the manifest's hashes name the kit's version, so a changed
// kit needs a regenerated manifest.
struct KitAdmissionConfig {
  // The kit this build was tested with: shown next to the installed one, never required.
  std::string expected_manifest_sha256;
  std::string required_architecture = "x86_64+wow64-i386";
  // Capabilities Playden can use; a kit's other capabilities are ignored.
  std::vector<std::string> known_capabilities;
  std::function<std::optional<std::string>(const std::filesystem::path&)> hash_file;
  std::function<std::optional<std::string>(std::string_view)> hash_bytes;
  // Digest of the reviewed publication-provider provenance JSON. Required when
  // playden-publication-guard-v1 is a known capability; never read from the kit.
  std::optional<std::string> publication_provider_sha256;
};

enum class KitManifestStatus {
  kVerified,
  kMalformed,
  kIntegrityFailure,
  kUnsupportedSchema,
  kUnsupportedKit,
  // Values are logged as numbers; 5 was the firmware check, removed when any firmware was allowed.
  kUnsupportedArchitecture = 6,
  kUnsupportedSource = 7,
};

struct KitManifestResult {
  KitManifestStatus status = KitManifestStatus::kMalformed;
  std::string reason;
  std::optional<VerifiedKitView> view;
  std::vector<std::string> firmware;
  std::vector<std::string> capabilities;
  std::vector<std::string> available_variant_ids;
  std::vector<RuntimeArtifactIdentity> artifacts;
  // What the kit says about itself, for display.
  std::string manifest_sha256;
  std::vector<std::string> ignored_capabilities;
  std::map<std::string, std::map<std::string, std::string>> sources;
  bool source_dirty = false;  // a title was built from uncommitted source
};

KitManifestResult VerifyKitManifest(const std::filesystem::path& root,
                                    const KitAdmissionConfig& config);

}  // namespace playden::runtime
