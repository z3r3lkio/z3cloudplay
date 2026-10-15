#pragma once

#include "homebrew/artwork.hpp"

#include <memory>
#include <string>
#include <vector>

namespace z3cloudplay::steam {
class HttpClient;
}

namespace z3cloudplay::homebrew {

inline constexpr std::size_t kMaxArtworkTransferBytes = 4 * 1024 * 1024;
// Bounds stb's simultaneous internal allocations. The validated returned RGBA
// vector is separately bounded by kMaxArtworkImageBytes.
inline constexpr std::size_t kMaxArtworkDecodeAllocationBytes = 16 * 1024 * 1024;

std::optional<ArtworkImage> DecodeArtwork(
    const std::vector<std::uint8_t>& encoded);

namespace artwork_internal {
// Deterministic budget seam for allocator-bound tests. Production callers use
// DecodeArtwork and its fixed kMaxArtworkDecodeAllocationBytes ceiling.
std::optional<ArtworkImage> DecodeArtworkWithAllocationBudget(
    const std::vector<std::uint8_t>& encoded, std::size_t allocation_limit);
}

class HttpArtworkBackend final : public ArtworkBackend {
 public:
  explicit HttpArtworkBackend(std::string ca_bundle);
  ~HttpArtworkBackend() override;
  HttpArtworkBackend(const HttpArtworkBackend&) = delete;
  HttpArtworkBackend& operator=(const HttpArtworkBackend&) = delete;

  std::optional<ArtworkImage> Load(
      const ArtworkKey&, const std::atomic<bool>& cancel) override;

 private:
  std::string ca_bundle_;
  std::unique_ptr<steam::HttpClient> http_;
};

}  // namespace z3cloudplay::homebrew
