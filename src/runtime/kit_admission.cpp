#include "runtime/kit_admission.h"
#include "runtime/artifact_hash.h"

namespace playden::runtime {
KitManifestResult VerifyInstalledKit(const std::filesystem::path& root,
                                    const KitAdmissionConfig& config) {
  auto production = config;
  production.hash_file = HashArtifactFile;
  production.hash_bytes = HashArtifactBytes;
  return VerifyKitManifest(root, production);
}
}  // namespace playden::runtime
