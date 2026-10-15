#include <mbedtls/build_info.h>
#include <mbedtls/threading.h>
#include <psa/crypto.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {
void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
}

int main() {
#if !defined(MBEDTLS_THREADING_C) || !defined(MBEDTLS_THREADING_PTHREAD)
  Check(false, "Concurrent TLS requires the Mbed TLS pthread configuration");
#else
  mbedtls_threading_mutex_t mutex;
  mbedtls_mutex_init(&mutex);
  unsigned counter = 0;
  Check(psa_crypto_init() == PSA_SUCCESS, "PSA initialization");
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 8; ++worker) workers.emplace_back([&, worker] {
    for (unsigned iteration = 0; iteration < 500; ++iteration) {
      Check(mbedtls_mutex_lock(&mutex) == 0, "Mbed TLS mutex lock");
      ++counter;
      Check(mbedtls_mutex_unlock(&mutex) == 0, "Mbed TLS mutex unlock");
      std::array<unsigned char, 16> key{};
      key[0] = worker; key[1] = iteration; key[2] = iteration >> 8;
      psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
      psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
      psa_set_key_bits(&attributes, 128);
      psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);
      mbedtls_svc_key_id_t id;
      Check(psa_import_key(&attributes, key.data(), key.size(), &id) == PSA_SUCCESS, "Concurrent key import");
      std::array<unsigned char, 16> exported{};
      std::size_t size = 0;
      Check(psa_export_key(id, exported.data(), exported.size(), &size) == PSA_SUCCESS,
            "Concurrent key export");
      Check(size == key.size() && exported == key, "Each key keeps its own bytes");
      Check(psa_destroy_key(id) == PSA_SUCCESS, "Concurrent key destruction");
      psa_reset_key_attributes(&attributes);
    }
  });
  for (auto& worker : workers) worker.join();
  Check(counter == 4000, "Mbed TLS pthread mutex synchronizes workers");
  mbedtls_mutex_free(&mutex);
#endif
}
