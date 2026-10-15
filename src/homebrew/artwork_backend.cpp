#include "homebrew/artwork_backend.hpp"

#include "steam/backend/http_client.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace {

struct DecodeBudget {
  std::size_t limit = 0;
  std::size_t outstanding = 0;
};

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size = 0;
  DecodeBudget* owner = nullptr;
};

thread_local DecodeBudget* g_decode_budget = nullptr;

void* ArtworkAllocate(std::size_t size) {
  DecodeBudget* budget = g_decode_budget;
  if (!budget || size > budget->limit -
                            std::min(budget->outstanding, budget->limit) ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
    return nullptr;
  auto* header = static_cast<AllocationHeader*>(
      std::malloc(sizeof(AllocationHeader) + size));
  if (!header) return nullptr;
  header->size = size;
  header->owner = budget;
  budget->outstanding += size;
  return header + 1;
}

void ArtworkFree(void* allocation) {
  if (!allocation) return;
  auto* header = static_cast<AllocationHeader*>(allocation) - 1;
  if (header->owner)
    header->owner->outstanding -=
        std::min(header->size, header->owner->outstanding);
  std::free(header);
}

void* ArtworkReallocate(void* allocation, std::size_t size) {
  if (!allocation) return ArtworkAllocate(size);
  if (size == 0) {
    ArtworkFree(allocation);
    return nullptr;
  }
  auto* old_header = static_cast<AllocationHeader*>(allocation) - 1;
  DecodeBudget* budget = old_header->owner;
  if (!budget ||
      (size > old_header->size &&
       size - old_header->size >
           budget->limit - std::min(budget->outstanding, budget->limit)) ||
      size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader))
    return nullptr;
  auto* header = static_cast<AllocationHeader*>(
      std::realloc(old_header, sizeof(AllocationHeader) + size));
  if (!header) return nullptr;
  if (size >= header->size) budget->outstanding += size - header->size;
  else budget->outstanding -= header->size - size;
  header->size = size;
  header->owner = budget;
  return header + 1;
}

class DecodeBudgetScope {
 public:
  explicit DecodeBudgetScope(std::size_t limit)
      : prior_(g_decode_budget), budget_{limit, 0} {
    g_decode_budget = &budget_;
  }
  ~DecodeBudgetScope() { g_decode_budget = prior_; }

 private:
  DecodeBudget* prior_;
  DecodeBudget budget_;
};

}  // namespace

#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MALLOC(size) ArtworkAllocate(size)
#define STBI_REALLOC(pointer, size) ArtworkReallocate(pointer, size)
#define STBI_FREE(pointer) ArtworkFree(pointer)
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace z3cloudplay::homebrew {
namespace {

constexpr std::uint32_t kArtworkTimeoutMs = 10'000;

bool CheckedImageBytes(int width, int height, std::size_t& bytes) {
  if (width <= 0 || height <= 0 ||
      width > static_cast<int>(kMaxArtworkDimension) ||
      height > static_cast<int>(kMaxArtworkDimension)) return false;
  const std::uint64_t value = static_cast<std::uint64_t>(width) *
                              static_cast<std::uint64_t>(height) * 4u;
  if (value > kMaxArtworkImageBytes ||
      value > std::numeric_limits<std::size_t>::max()) return false;
  bytes = static_cast<std::size_t>(value);
  return true;
}

// The game's own CDN art: its cover, its library hero and its logo, and nothing else.
bool CanonicalKey(const ArtworkKey& key) {
  if (key.account_generation == 0 || key.game_id == 0 ||
      key.source.size() > kMaxArtworkSourceBytes) return false;
  const std::string base = "https://cdn.cloudflare.steamstatic.com/steam/apps/" +
                           std::to_string(key.game_id) + "/";
  for (const char* name : {"library_600x900.jpg", "library_hero.jpg", "logo.png"})
    if (key.source == base + name) return true;
  return false;
}

}  // namespace

std::optional<ArtworkImage> artwork_internal::DecodeArtworkWithAllocationBudget(
    const std::vector<std::uint8_t>& encoded, std::size_t allocation_limit) {
  if (encoded.empty() || encoded.size() > kMaxArtworkTransferBytes ||
      encoded.size() > static_cast<std::size_t>(INT_MAX) || allocation_limit == 0 ||
      allocation_limit > kMaxArtworkDecodeAllocationBytes) return std::nullopt;
  DecodeBudgetScope budget(allocation_limit);

  int width = 0;
  int height = 0;
  int components = 0;
  if (!stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                             &width, &height, &components)) return std::nullopt;
  std::size_t bytes = 0;
  if (!CheckedImageBytes(width, height, bytes)) return std::nullopt;

  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_components = 0;
  stbi_uc* decoded = stbi_load_from_memory(
      encoded.data(), static_cast<int>(encoded.size()), &decoded_width,
      &decoded_height, &decoded_components, 4);
  if (!decoded) return std::nullopt;
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(decoded,
                                                              stbi_image_free);
  std::size_t decoded_bytes = 0;
  if (decoded_width != width || decoded_height != height ||
      !CheckedImageBytes(decoded_width, decoded_height, decoded_bytes) ||
      decoded_bytes != bytes) return std::nullopt;

  ArtworkImage image;
  image.width = static_cast<std::uint32_t>(decoded_width);
  image.height = static_cast<std::uint32_t>(decoded_height);
  image.rgba.assign(pixels.get(), pixels.get() + decoded_bytes);
  return image;
}

std::optional<ArtworkImage> DecodeArtwork(
    const std::vector<std::uint8_t>& encoded) {
  return artwork_internal::DecodeArtworkWithAllocationBudget(
      encoded, kMaxArtworkDecodeAllocationBytes);
}

HttpArtworkBackend::HttpArtworkBackend(std::string ca_bundle)
    : ca_bundle_(std::move(ca_bundle)) {}

HttpArtworkBackend::~HttpArtworkBackend() = default;

std::optional<ArtworkImage> HttpArtworkBackend::Load(
    const ArtworkKey& key, const std::atomic<bool>& cancel) {
  if (!CanonicalKey(key) || cancel.load(std::memory_order_acquire))
    return std::nullopt;
  if (!http_) http_ = std::make_unique<steam::HttpClient>(ca_bundle_);

  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = key.source;
  request.timeout_ms = kArtworkTimeoutMs;
  request.max_response_bytes = kMaxArtworkTransferBytes;
  auto response = http_->Perform(request, cancel);
  if (!response.ok() || response.status_code != 200) return std::nullopt;
  return DecodeArtwork(response.body);
}

}  // namespace z3cloudplay::homebrew
