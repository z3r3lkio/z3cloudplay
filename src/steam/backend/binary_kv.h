#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace z3cloudplay::steam {

struct PackageContents {
  std::vector<std::uint32_t> app_ids;
  std::vector<std::uint32_t> depot_ids;
};

// Parses a PICS package buffer: u32 LE 1, then binary KeyValues whose single root key is the
// decimal package id. Returns false on any malformed input or identity mismatch.
bool ParsePackageBuffer(const std::uint8_t* data, std::size_t size, std::uint32_t package_id,
                        PackageContents* out);

}  // namespace z3cloudplay::steam
