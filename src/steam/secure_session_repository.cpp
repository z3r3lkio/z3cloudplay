#include "steam/secure_session_repository.h"
#include "steam/secure_session_vault.h"
#include <cstdint>
#include <limits>
#include <mbedtls/platform_util.h>

namespace z3cloudplay::security {
namespace {
constexpr const char* kPath="session.v2";
constexpr const char* kTemp="session.v2.tmp";
constexpr std::size_t kLimit=64*1024;
constexpr std::size_t kTokenLimit=24*1024;
constexpr std::size_t kNameLimit=256;
struct SecretStringGuard {
  std::string& value;
  ~SecretStringGuard() {
    if (!value.empty()) mbedtls_platform_zeroize(&value[0], value.size());
  }
};
struct SecretBytesGuard {
  Bytes& value;
  ~SecretBytesGuard() {
    if (!value.empty()) mbedtls_platform_zeroize(value.data(), value.size());
  }
};
template <typename T> Result<T> Failure() {
  return Result<T>::Failure(StorageError::kInvalidData,"Secure session unavailable or invalid");
}
Result<void> FailSave() {
  return Result<void>::Failure(StorageError::kInvalidData,"Secure session unavailable or invalid");
}
void U32(Bytes& dst, std::uint32_t value) {
  for(int n=0;n<4;++n) dst.push_back(static_cast<std::uint8_t>(value>>(n*8)));
}
void U64(Bytes& dst, std::uint64_t value) {
  for(int n=0;n<8;++n) dst.push_back(static_cast<std::uint8_t>(value>>(n*8)));
}
void Str(Bytes& dst,const std::string& s) {
  U32(dst,static_cast<std::uint32_t>(s.size()));
  dst.insert(dst.end(),s.begin(),s.end());
}
class Parser {
 public:
  explicit Parser(const Bytes& source) : source_(source) {}
  bool U32(std::uint32_t& v) {
    if(source_.size()-pos_<4) return false;
    v=0;
    for(unsigned i=0;i<4;++i) v|=std::uint32_t(source_[pos_++])<<(8*i);
    return true;
  }
  bool U64(std::uint64_t& v) {
    if(source_.size()-pos_<8) return false;
    v=0;
    for(unsigned i=0;i<8;++i) v|=std::uint64_t(source_[pos_++])<<(8*i);
    return true;
  }
  bool Str(std::string& out, std::size_t limit) {
    std::uint32_t size=0;
    if(!U32(size) || size>limit || size>source_.size()-pos_) return false;
    out.assign(reinterpret_cast<const char*>(source_.data()+pos_),size);
    pos_+=size;
    return true;
  }
  bool Done() const {return pos_==source_.size();}
 private:
  const Bytes& source_;
  std::size_t pos_=0;
};
}
Result<void> SecureSessionRepository::Save(const StoredSession& session) const {
  if(!unlock_) return FailSave();
  auto pass=unlock_();
  SecretStringGuard pass_guard{pass};
  if(pass.size()<12 || session.steam_id==0 || session.refresh_token.empty() ||
     session.account_name.size()>kNameLimit ||
     session.refresh_token.size()>kTokenLimit ||
     session.access_token.size()>kTokenLimit) return FailSave();
  Bytes payload;
  SecretBytesGuard payload_guard{payload};
  U32(payload,2);
  U64(payload,session.steam_id);
  U32(payload,session.cell_id?1:0);
  if(session.cell_id) U32(payload,*session.cell_id);
  Str(payload,session.account_name);
  Str(payload,session.refresh_token);
  Str(payload,session.access_token);
  if(payload.size()>kLimit) return FailSave();
  auto encrypted=SealSession(payload,pass);
  if(!encrypted.ok()) return FailSave();
  return storage_.WriteAtomicPrivate(kPath,encrypted.value());
}
Result<std::optional<StoredSession>> SecureSessionRepository::Load() const {
  // Legacy plaintext is never loaded by this repository.
  auto data=storage_.Read(kPath,kLimit+64);
  if(!data.ok()) {
    if(data.error()==StorageError::kNotFound)
      return Result<std::optional<StoredSession>>::Success(std::nullopt);
    return Failure<std::optional<StoredSession>>();
  }
  if(!unlock_) return Failure<std::optional<StoredSession>>();
  const auto pass=unlock_();
  if(pass.size()<12) return Failure<std::optional<StoredSession>>();
  auto plaintext=OpenSession(data.value(),pass);
  if(!plaintext.ok()) return Failure<std::optional<StoredSession>>();
  SecretBytesGuard plaintext_guard{plaintext.value()};
  Parser reader(plaintext.value());
  std::uint32_t version=0,cell_flag=0;
  StoredSession result;
  if(!reader.U32(version) || version!=2 || !reader.U64(result.steam_id) ||
     !reader.U32(cell_flag) || cell_flag>1) return Failure<std::optional<StoredSession>>();
  if(cell_flag) {
    std::uint32_t cell=0;
    if(!reader.U32(cell)) return Failure<std::optional<StoredSession>>();
    result.cell_id=cell;
  }
  if(!reader.Str(result.account_name,kNameLimit) ||
     !reader.Str(result.refresh_token,kTokenLimit) ||
     !reader.Str(result.access_token,kTokenLimit) ||
     !reader.Done() || result.steam_id==0 || result.refresh_token.empty())
    return Failure<std::optional<StoredSession>>();
  return Result<std::optional<StoredSession>>::Success(std::move(result));
}
Result<void> SecureSessionRepository::Clear() const {
  auto a=storage_.Remove(kPath);
  auto b=storage_.Remove(kTemp);
  // Sign-out also removes any legacy cleartext remnants.
  auto c=storage_.Remove("session.v1");
  auto d=storage_.Remove("session.v1.tmp");
  if(!a.ok()) return a;
  if(!b.ok()) return b;
  if(!c.ok()) return c;
  return d;
}
}  // namespace z3cloudplay::security
