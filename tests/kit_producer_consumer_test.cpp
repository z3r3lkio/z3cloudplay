#include "runtime/artifact_hash.h"
#include "runtime/kit_admission.h"
#include <cstdio>

// The kit the producer emits is admitted by the consumer, and reported as the manifest
// the producer hashed.
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  using namespace z3cloudplay::runtime;
  KitAdmissionConfig config;
  config.expected_manifest_sha256 = argv[2];
  config.known_capabilities = {"per_game_title", "offline_bottle", "wow64_i386", "dxvk"};
  const auto result = VerifyInstalledKit(argv[1], config);
  if (result.status != KitManifestStatus::kVerified) {
    std::fprintf(stderr, "admission rejected: %s\n", result.reason.c_str());
    return 1;
  }
  if (!result.view || result.available_variant_ids.size() != 1 ||
      result.available_variant_ids.front() != "hl2-fa78001-v1" || result.manifest_sha256 != argv[2] ||
      !result.ignored_capabilities.empty()) return 2;
  std::puts("verified emitted kit manifest with production SHA-256 admission");
  return 0;
}
