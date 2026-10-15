#pragma once

#include "runtime/kit_manifest.h"

namespace playden::runtime {

// Admission with the production SHA-256 and no-follow file hashing backend.
// Caller-supplied hash callbacks are ignored.
KitManifestResult VerifyInstalledKit(const std::filesystem::path& root,
                                    const KitAdmissionConfig& config);

}  // namespace playden::runtime
