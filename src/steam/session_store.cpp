#include "steam/session_store.h"

#include <algorithm>
#include <array>
#include <utility>

namespace z3cloudplay {
namespace {
constexpr char kPath[] = "session.v1";
constexpr char kTemporaryPath[] = "session.v1.tmp";
constexpr std::array<std::uint8_t, 8> kMagic{{'P', 'L', 'D', 'S', 'E', 'S', 'S', '1'}};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kHasCellId = 1;
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kMaximumSessionBytes = 64 * 1024;
constexpr std::size_t kMaximumAccountBytes = 256;
constexpr std::size_t kMaximumTokenBytes = 24 * 1024;

Result<void> Invalid(const char* reason) {
  return Result<void>::Failure(StorageError::kInvalidData,
                               std::string("session data is invalid: ") + reason);
}
template <typename T>
Result<T> InvalidValue(const char* reason) {
  return Result<T>::Failure(StorageError::kInvalidData,
                            std::string("session data is invalid: ") + reason);
}

void Put16(Bytes& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Put32(Bytes& output, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}
void Put64(Bytes& output, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}
void PutString(Bytes& output, const std::string& value) {
  Put32(output, static_cast<std::uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

std::uint32_t Checksum(const std::uint8_t* data, std::size_t size) {
  std::uint32_t value = 2166136261u;
  for (std::size_t i = 0; i < size; ++i) { value ^= data[i]; value *= 16777619u; }
  return value;
}

class Reader {
 public:
  Reader(const Bytes& bytes, std::size_t offset) : bytes_(bytes), offset_(offset) {}
  bool U16(std::uint16_t& value) { std::uint32_t wide = 0; if (!Integer(wide, 2)) return false; value = static_cast<std::uint16_t>(wide); return true; }
  bool U32(std::uint32_t& value) { return Integer(value, 4); }
  bool U64(std::uint64_t& value) { return Integer(value, 8); }
  bool String(std::string& value, std::size_t maximum) {
    std::uint32_t size = 0;
    if (!U32(size) || size > maximum || size > bytes_.size() - offset_) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size); offset_ += size; return true;
  }
  std::size_t offset() const { return offset_; }
 private:
  template <typename T> bool Integer(T& value, std::size_t size) {
    if (size > bytes_.size() - offset_) return false;
    value = 0; for (std::size_t i = 0; i < size; ++i) value |= static_cast<T>(bytes_[offset_++]) << (8 * i);
    return true;
  }
  const Bytes& bytes_; std::size_t offset_;
};
}

Result<void> SessionStore::Save(const StoredSession& session) const {
  if (session.steam_id == 0 || session.refresh_token.empty())
    return Invalid("required identity or refresh credential is missing");
  if (session.account_name.size() > kMaximumAccountBytes ||
      session.refresh_token.size() > kMaximumTokenBytes ||
      session.access_token.size() > kMaximumTokenBytes)
    return Invalid("field length exceeds limit");

  Bytes payload; payload.reserve(32 + session.account_name.size() + session.refresh_token.size() + session.access_token.size());
  Put64(payload, session.steam_id);
  if (session.cell_id) Put32(payload, *session.cell_id);
  PutString(payload, session.account_name); PutString(payload, session.refresh_token); PutString(payload, session.access_token);
  if (payload.size() > kMaximumSessionBytes - kHeaderBytes) return Invalid("envelope exceeds limit");

  Bytes encoded; encoded.reserve(kHeaderBytes + payload.size());
  encoded.insert(encoded.end(), kMagic.begin(), kMagic.end()); Put16(encoded, kVersion);
  Put16(encoded, session.cell_id ? kHasCellId : 0); Put32(encoded, static_cast<std::uint32_t>(payload.size()));
  Put32(encoded, Checksum(payload.data(), payload.size())); encoded.insert(encoded.end(), payload.begin(), payload.end());
  return storage_.WriteAtomicPrivate(kPath, encoded);
}

Result<std::optional<StoredSession>> SessionStore::Load() const {
  const auto stored = storage_.Read(kPath, kMaximumSessionBytes);
  if (!stored.ok()) {
    if (stored.error() == StorageError::kNotFound)
      return Result<std::optional<StoredSession>>::Success(std::nullopt);
    return Result<std::optional<StoredSession>>::Failure(stored.error(), stored.message());
  }
  const Bytes& bytes = stored.value();
  if (bytes.size() < kHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    return InvalidValue<std::optional<StoredSession>>("header");
  Reader header(bytes, kMagic.size()); std::uint16_t version = 0, flags = 0; std::uint32_t payload_size = 0, checksum = 0;
  if (!header.U16(version) || !header.U16(flags) || !header.U32(payload_size) || !header.U32(checksum) ||
      version != kVersion || (flags & ~kHasCellId) != 0 || payload_size != bytes.size() - kHeaderBytes ||
      Checksum(bytes.data() + kHeaderBytes, payload_size) != checksum)
    return InvalidValue<std::optional<StoredSession>>("envelope");

  Reader payload(bytes, kHeaderBytes); StoredSession session;
  if (!payload.U64(session.steam_id)) return InvalidValue<std::optional<StoredSession>>("payload");
  if (flags & kHasCellId) { std::uint32_t cell = 0; if (!payload.U32(cell)) return InvalidValue<std::optional<StoredSession>>("cell id"); session.cell_id = cell; }
  if (!payload.String(session.account_name, kMaximumAccountBytes) ||
      !payload.String(session.refresh_token, kMaximumTokenBytes) ||
      !payload.String(session.access_token, kMaximumTokenBytes) || payload.offset() != bytes.size())
    return InvalidValue<std::optional<StoredSession>>("fields");
  if (session.steam_id == 0 || session.refresh_token.empty())
    return InvalidValue<std::optional<StoredSession>>("required fields");
  return Result<std::optional<StoredSession>>::Success(std::move(session));
}

Result<void> SessionStore::Clear() const {
  const auto final_result = storage_.Remove(kPath);
  const auto temporary_result = storage_.Remove(kTemporaryPath);
  if (!final_result.ok()) return final_result;
  if (!temporary_result.ok()) return temporary_result;
  return Result<void>::Success();
}

}  // namespace z3cloudplay
