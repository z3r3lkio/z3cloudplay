#include "steam/secure_session_vault.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/platform_util.h>

namespace z3cloudplay::security {
namespace {
constexpr std::array<unsigned char,8> kMagic = {'Z','3','C','V','L','T','0','2'};
constexpr unsigned kRounds=600000;
constexpr size_t kSalt=16, kNonce=12, kTag=16, kKey=32, kHeader=8+4+kSalt+kNonce;
constexpr size_t kMaxData=64*1024;
Result<Bytes> Fail() {
  return Result<Bytes>::Failure(StorageError::kInvalidData, "Secure vault authentication or crypto operation failed");
}
void Put32(Bytes& b, unsigned v) {
  for (int i=0;i<4;++i) b.push_back(static_cast<unsigned char>(v>>(i*8)));
}
unsigned Get32(const Bytes& b) {
  return unsigned(b[8]) | (unsigned(b[9])<<8) | (unsigned(b[10])<<16) | (unsigned(b[11])<<24);
}
bool Derive(const std::string& pass, const unsigned char* salt, unsigned rounds, unsigned char* key) {
  if (pass.size()<12 || pass.size()>1024 || rounds!=kRounds) return false;
  const mbedtls_md_info_t* md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  bool ok=mbedtls_md_setup(&ctx,md,1)==0;
  if(ok) ok=mbedtls_pkcs5_pbkdf2_hmac(&ctx,
    reinterpret_cast<const unsigned char*>(pass.data()),pass.size(),
    salt,kSalt,rounds,kKey,key)==0;
  mbedtls_md_free(&ctx);
  return ok;
}
}  // namespace

Result<Bytes> SealSession(const Bytes& plaintext, const std::string& passphrase) {
  if (plaintext.empty() || plaintext.size()>kMaxData || passphrase.size()<12 || passphrase.size()>1024) return Fail();
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  constexpr unsigned char personalization[]="z3cloudplay-session-v2";
  bool ok=mbedtls_ctr_drbg_seed(&drbg,mbedtls_entropy_func,&entropy,
       personalization,sizeof(personalization)-1)==0;
  std::array<unsigned char,kSalt+kNonce> random{};
  if(ok) ok=mbedtls_ctr_drbg_random(&drbg,random.data(),random.size())==0;
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  if(!ok) return Fail();

  Bytes output(kMagic.begin(),kMagic.end());
  Put32(output,kRounds);
  output.insert(output.end(),random.begin(),random.end());
  std::array<unsigned char,kKey> key{};
  ok=Derive(passphrase,random.data(),kRounds,key.data());
  if (!ok) {mbedtls_platform_zeroize(key.data(),key.size());return Fail();}
  output.resize(kHeader+plaintext.size()+kTag);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  ok=mbedtls_gcm_setkey(&gcm,MBEDTLS_CIPHER_ID_AES,key.data(),256)==0;
  if(ok) ok=mbedtls_gcm_crypt_and_tag(&gcm,MBEDTLS_GCM_ENCRYPT,plaintext.size(),
    random.data()+kSalt,kNonce,output.data(),kHeader,
    plaintext.data(),output.data()+kHeader,kTag,
    output.data()+kHeader+plaintext.size())==0;
  mbedtls_gcm_free(&gcm);
  mbedtls_platform_zeroize(key.data(),key.size());
  if(!ok) return Fail();
  return Result<Bytes>::Success(std::move(output));
}

Result<Bytes> OpenSession(const Bytes& sealed, const std::string& passphrase) {
  if(sealed.size()<kHeader+kTag+1 || sealed.size()>kHeader+kTag+kMaxData ||
     !std::equal(kMagic.begin(),kMagic.end(),sealed.begin()) || Get32(sealed)!=kRounds)
    return Fail();
  const size_t ciphertext_len=sealed.size()-kHeader-kTag;
  std::array<unsigned char,kKey> key{};
  bool ok=Derive(passphrase,sealed.data()+12,Get32(sealed),key.data());
  if(!ok) {mbedtls_platform_zeroize(key.data(),key.size());return Fail();}
  Bytes plain(ciphertext_len);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  ok=mbedtls_gcm_setkey(&gcm,MBEDTLS_CIPHER_ID_AES,key.data(),256)==0;
  if(ok) ok=mbedtls_gcm_auth_decrypt(&gcm,ciphertext_len,
     sealed.data()+12+kSalt,kNonce,sealed.data(),kHeader,
     sealed.data()+kHeader+ciphertext_len,kTag,
     sealed.data()+kHeader,plain.data())==0;
  mbedtls_gcm_free(&gcm);
  mbedtls_platform_zeroize(key.data(),key.size());
  if(!ok) {
    if(!plain.empty()) mbedtls_platform_zeroize(plain.data(),plain.size());
    return Fail();
  }
  return Result<Bytes>::Success(std::move(plain));
}
}  // namespace z3cloudplay::security
