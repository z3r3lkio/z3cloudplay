#pragma once

// A small 64-bit (or 32-bit) program wrapped the way SteamStub 3.1 wraps BALL x PIT's Balls.exe: code
// section encrypted (AES-256-CBC, first block kept in the header), entry point in a last
// ".bind" section preceded by the XOR-chained 0xF0-byte header.

#include <algorithm>
#include <cstdint>
#include <string>

#include <mbedtls/aes.h>

namespace steam_stub_fixture {

struct Spec {
  std::uint32_t flags = 0;
  std::uint32_t signature = 0xC0DEC0DF;
  std::uint32_t app_id = 2062430;
  bool reloc_into_bind = false;  // a relocation block for a .bind page (keeps .bind)
  bool pe32 = false;            // a 32-bit program (PE32, image base 0x400000)
  std::uint64_t image_base = 0;  // 0: the usual base for the bitness
  std::size_t header30 = 0;      // 0xD0 or 0xB0: a variant 3.0 header (signature 0xC0DEC0DE) instead of 3.1
  bool tls = false;              // the wrapper starts from the first TLS callback; the entry point is the game's
  bool tls_replaced = false;     // 3.0 x64: the callback replaced the game's own, the entry point hides the real one
};

constexpr std::uint32_t kOriginalCallback = 0x1020;  // the game's own TLS callback (tls_replaced)
constexpr std::uint32_t kStubEntry = 0x3000 + 0x20;  // the stub's entry function (tls_replaced)
constexpr std::uint32_t kTlsDirectory = 0x2100, kTlsCallbacks = 0x2140;

constexpr std::uint32_t kOriginalEntry = 0x1010;
constexpr std::size_t kCodeRaw = 0x400, kCodeSize = 0x200;
constexpr std::size_t kRelocRaw = 0x600;
constexpr std::size_t kBindRaw = 0x800, kBindSize = 0x200;
constexpr std::uint32_t kBindEntry = 0x3000 + 0x100;

inline void Put(std::string* bytes, std::size_t at, std::uint64_t value, int size) {
  for (int i = 0; i < size; ++i) (*bytes)[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
}
inline std::uint32_t Get32(const std::string& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i) value = value << 8 | static_cast<unsigned char>(bytes[at + i]);
  return value;
}

// The code section as built, before encryption.
inline std::string PlainCode() {
  std::string code(kCodeSize, '\0');
  for (std::size_t i = 0; i < code.size(); ++i) code[i] = static_cast<char>((i * 7 + 3) & 0xff);
  return code;
}

constexpr std::size_t kPe = 0x40;
constexpr std::size_t kOptional = kPe + 24;
constexpr std::size_t kOptionalSize = 0xf0;
constexpr std::size_t kTable = kOptional + kOptionalSize;

inline std::string Build(const Spec& spec = {}) {
  std::string bytes(kBindRaw + kBindSize, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; Put(&bytes, 0x3c, kPe, 4);
  bytes[kPe] = 'P'; bytes[kPe + 1] = 'E';
  Put(&bytes, kPe + 4, spec.pe32 ? 0x014c : 0x8664, 2);
  Put(&bytes, kPe + 6, 3, 2);
  Put(&bytes, kPe + 20, kOptionalSize, 2);
  const std::uint64_t image_base = spec.image_base ? spec.image_base : spec.pe32 ? 0x400000 : 0x140000000ull;
  const std::size_t directories = spec.pe32 ? 96 : 112;
  Put(&bytes, kOptional, spec.pe32 ? 0x10b : 0x20b, 2);
  if (spec.pe32) Put(&bytes, kOptional + 28, image_base, 4);
  else Put(&bytes, kOptional + 24, image_base, 8);
  Put(&bytes, kOptional + 16, kBindEntry, 4);
  Put(&bytes, kOptional + 32, 0x1000, 4);  // section alignment
  Put(&bytes, kOptional + 56, 0x4000, 4);  // size of image
  Put(&bytes, kOptional + directories + 5 * 8, 0x2000, 4);  // relocations
  Put(&bytes, kOptional + directories + 5 * 8 + 4, 0x10, 4);
  const auto section = [&](int row, const char* name, std::uint32_t virtual_size, std::uint32_t address,
                           std::uint32_t raw_size, std::uint32_t raw) {
    const auto at = kTable + row * 40;
    for (int i = 0; name[i]; ++i) bytes[at + i] = name[i];
    Put(&bytes, at + 8, virtual_size, 4);
    Put(&bytes, at + 12, address, 4);
    Put(&bytes, at + 16, raw_size, 4);
    Put(&bytes, at + 20, raw, 4);
  };
  section(0, ".text", 0x1f0, 0x1000, kCodeSize, kCodeRaw);
  section(1, ".reloc", 0x10, 0x2000, 0x200, kRelocRaw);
  section(2, ".bind", kBindSize, 0x3000, kBindSize, kBindRaw);
  Put(&bytes, kRelocRaw, spec.reloc_into_bind ? 0x3000 : 0x1000, 4);
  Put(&bytes, kRelocRaw + 4, 0x10, 4);

  unsigned char key[32], iv[16], encrypted_iv[16];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<unsigned char>(0xa0 + i);
  for (int i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(0x30 + 3 * i);
  std::string plain = PlainCode() + std::string(16, '\x5a');  // the wrapper encrypts one block more
  std::string cipher(plain.size(), '\0');
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 256);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, iv, encrypted_iv);
  unsigned char chain[16];
  std::copy(iv, iv + 16, chain);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, plain.size(), chain,
                        reinterpret_cast<const unsigned char*>(plain.data()),
                        reinterpret_cast<unsigned char*>(cipher.data()));
  mbedtls_aes_free(&aes);
  bytes.replace(kCodeRaw, kCodeSize, cipher.substr(16));

  if (spec.flags & 0x04) bytes.replace(kCodeRaw, kCodeSize, PlainCode());  // not encrypted
  constexpr std::uint32_t kSeed = 0x13579bdf;
  std::string header(spec.header30 ? spec.header30 : 0xF0, '\0');
  // What the header names as the original entry: the game's entry, or its replaced TLS callback.
  const std::uint32_t original = spec.tls_replaced ? kOriginalCallback : kOriginalEntry;
  Put(&header, 0x00, kSeed, 4);
  if (spec.header30) {
    Put(&header, 4, spec.signature == 0xC0DEC0DF ? 0xC0DEC0DE : spec.signature, 4);
    Put(&header, 8, image_base, 8);
    Put(&header, 16, kBindEntry, 4);
    Put(&header, 28, original, 4);
    Put(&header, 48, spec.app_id, 4);
    Put(&header, 52, spec.flags, 4);
    Put(&header, 64, 0x1000, 4);
    Put(&header, 68, kCodeSize, 4);
    header.replace(72, 32, reinterpret_cast<const char*>(key), 32);
    header.replace(104, 16, reinterpret_cast<const char*>(encrypted_iv), 16);
    header.replace(120, 16, cipher.substr(0, 16));
    Put(&header, 152, spec.tls_replaced ? 1 : 0, 4);
  } else {
    Put(&header, 0x04, spec.signature, 4);
    Put(&header, 0x08, image_base, 8);
    Put(&header, 0x10, kBindEntry, 8);
    Put(&header, 0x20, original, 8);
    Put(&header, 0x38, spec.app_id, 4);
    Put(&header, 0x3C, spec.flags, 4);
    Put(&header, 0x48, 0x1000, 8);
    Put(&header, 0x50, kCodeSize, 8);
    header.replace(0x58, 32, reinterpret_cast<const char*>(key), 32);
    header.replace(0x78, 16, reinterpret_cast<const char*>(encrypted_iv), 16);
    header.replace(0x88, 16, cipher.substr(0, 16));
  }
  std::string stored = header;
  for (std::size_t at = 4; at < header.size(); at += 4)
    Put(&stored, at, Get32(header, at) ^ Get32(stored, at - 4), 4);
  const std::size_t entry_file = kBindRaw + (kBindEntry - 0x3000);
  bytes.replace(entry_file - header.size(), header.size(), stored);

  if (spec.tls) {
    // TLS directory and callback array in .reloc's spare space; the first callback is the wrapper.
    const std::size_t directories = spec.pe32 ? 96 : 112;
    Put(&bytes, kOptional + directories + 9 * 8, kTlsDirectory, 4);
    Put(&bytes, kOptional + directories + 9 * 8 + 4, spec.pe32 ? 0x18 : 0x28, 4);
    const std::size_t directory_file = kRelocRaw + (kTlsDirectory - 0x2000);
    const std::size_t callbacks_file = kRelocRaw + (kTlsCallbacks - 0x2000);
    Put(&bytes, directory_file + (spec.pe32 ? 12 : 24), image_base + kTlsCallbacks, spec.pe32 ? 4 : 8);
    Put(&bytes, callbacks_file, image_base + kBindEntry, spec.pe32 ? 4 : 8);
    if (spec.tls_replaced) {
      // The stub's entry: sub rdx, imm32 ; mov edx, [rdx] ; xor edx, key, where key ^ seed is the
      // distance from the stub's entry to the game's.
      const unsigned char code[] = {0x48, 0x81, 0xEA, 0x10, 0, 0, 0, 0x8B, 0x12, 0x81, 0xF2};
      const std::size_t stub_file = kBindRaw + (kStubEntry - 0x3000);
      bytes.replace(stub_file, sizeof(code), reinterpret_cast<const char*>(code), sizeof(code));
      Put(&bytes, stub_file + sizeof(code), kSeed ^ (kOriginalEntry - kStubEntry), 4);
      Put(&bytes, kOptional + 16, kStubEntry, 4);
    } else {
      Put(&bytes, kOptional + 16, kOriginalEntry, 4);  // the game's own entry point stays
    }
  }
  return bytes;
}

// A 32-bit program wrapped the way SteamStub 2.1 wraps Blades of Time: the start function in
// ".bind" (after a 0xC0DEC0DE marker) names an XOR-chained header, the header an XOR-chained
// payload, the payload an XTEA-encrypted SteamDRMP.dll whose code says where the payload keeps
// the entry point, code section and AES key.
struct Spec21 {
  std::uint32_t flags = 0x20;
  std::uint32_t app_id = 208670;
  bool d0_header = false;  // the 0xD0-dword header, without LoadLibraryW
  // SteamDRMP.dll builds differ after the fifth field load: Blades of Time stores then takes
  // the key and IV straight away; Final Fantasy XIII pushes registers and loads a global in
  // between, which moves the lea and the add.
  bool pushes_in_tail = false;
};

constexpr std::size_t kBind21Size = 0x1000;
constexpr std::uint32_t kBind21Entry = 0x3000 + 0xC00;
constexpr std::uint32_t kImage21 = 0x400000;

inline void Xor21(std::string* bytes, std::size_t at, std::size_t size, std::uint32_t* key) {
  for (std::size_t i = at; i < at + size; i += 4) {
    const std::uint32_t stored = Get32(*bytes, i) ^ *key;
    Put(bytes, i, stored, 4);
    *key = stored;
  }
}

inline std::string Build21(const Spec21& spec = {}) {
  Spec base;
  base.pe32 = true;
  std::string bytes = Build(base);  // the 3.1 skeleton: headers, .text, .reloc
  bytes.resize(kBindRaw + kBind21Size, '\0');
  std::fill(bytes.begin() + kBindRaw, bytes.end(), '\0');
  Put(&bytes, kTable + 2 * 40 + 8, kBind21Size, 4);
  Put(&bytes, kTable + 2 * 40 + 16, kBind21Size, 4);
  Put(&bytes, kOptional + 16, kBind21Entry, 4);
  Put(&bytes, kOptional + 56, 0x4000 + kBind21Size - 0x1000, 4);

  unsigned char key[32], iv[16], encrypted_iv[16];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<unsigned char>(0x11 + 5 * i);
  for (int i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(0x70 + i);
  const std::string plain = PlainCode() + std::string(16, '\x6b');
  std::string cipher(plain.size(), '\0');
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 256);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, iv, encrypted_iv);
  unsigned char chain[16];
  std::copy(iv, iv + 16, chain);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, plain.size(), chain, reinterpret_cast<const unsigned char*>(plain.data()),
                        reinterpret_cast<unsigned char*>(cipher.data()));
  mbedtls_aes_free(&aes);
  const bool encrypted = (spec.flags & 0x04) == 0;
  if (encrypted) bytes.replace(kCodeRaw, kCodeSize, cipher.substr(16));
  else bytes.replace(kCodeRaw, kCodeSize, PlainCode());

  // .bind: header at +0, payload at +0x380, SteamDRMP.dll at +0x500, start function at +0xC00.
  constexpr std::size_t header_at = kBindRaw, payload_at = kBindRaw + 0x380, drmp_at = kBindRaw + 0x500;
  constexpr std::size_t header_size = 0x30 * 4, payload_size = 0x100, drmp_size = 0x500;
  const std::size_t header_size_used = spec.d0_header ? 0xD0 * 4 : header_size;
  std::string header(header_size_used, '\0');
  const std::size_t shift = spec.d0_header ? 4 : 0;
  Put(&header, 36 - shift, kImage21 + 0x3380, 4);  // payload address
  Put(&header, 40 - shift, payload_size, 4);
  Put(&header, 44 - shift, spec.app_id, 4);
  Put(&header, 60 - shift, 0x50, 4);  // where the payload keeps SteamDRMP.dll's address
  Put(&header, 64 - shift, 0x54, 4);  // ... and its size
  Put(&header, 68 - shift, 0x60, 4);  // ... and the XTEA keys (to the payload's end)
  std::string payload(payload_size, '\0');
  Put(&payload, 0x00, spec.flags, 4);
  Put(&payload, 0x04, kImage21 + kOriginalEntry, 4);
  Put(&payload, 0x08, kImage21 + 0x1000, 4);
  Put(&payload, 0x0C, kCodeSize, 4);
  payload.replace(0x10, 32, reinterpret_cast<const char*>(key), 32);
  payload.replace(0x30, 16, reinterpret_cast<const char*>(encrypted_iv), 16);
  payload.replace(0x40, 16, cipher.substr(0, 16));
  Put(&payload, 0x50, kImage21 + 0x3500, 4);
  Put(&payload, 0x54, drmp_size, 4);
  for (std::size_t at = 0x60; at < payload_size; at += 4) Put(&payload, at, 0x01020304u * (at / 4), 4);

  // SteamDRMP.dll's block of five "mov reg, [payload+x]; mov [y], reg" pairs, then
  // "lea reg, [payload+key]" and "add reg, iv" (Steamless's first pattern); the unpacker reads
  // the payload offsets from the operands.
  std::string drmp(drmp_size, '\0');
  constexpr std::size_t block = 0x40;
  // mov edx, [eax+disp32] (8B 90) and mov [abs32], edx (89 15), as the real builds encode them.
  for (int i = 0; i < 5; ++i) {
    drmp[block + 12 * i] = '\x8B';
    drmp[block + 12 * i + 1] = '\x90';
    drmp[block + 12 * i + 6] = '\x89';
    drmp[block + 12 * i + 7] = '\x15';
  }
  Put(&drmp, block + 2, 0x00, 4);   // flags
  Put(&drmp, block + 14, 0xF0, 4);  // app id (unused)
  Put(&drmp, block + 26, 0x04, 4);  // original entry point
  Put(&drmp, block + 38, 0x08, 4);  // code section address
  Put(&drmp, block + 50, 0x0C, 4);  // encrypted size
  if (!spec.pushes_in_tail) {
    drmp[block + 60] = '\x8D'; drmp[block + 61] = '\x90';  // lea edx, [eax+key]
    Put(&drmp, block + 62, 0x10, 4);  // AES key
    drmp[block + 66] = '\x05';                                // add eax, iv
    Put(&drmp, block + 67, 0x30, 4);  // AES IV, then the code's first block
  } else {
    // ... 8B 90 <size> | 53 | 89 15 <abs> | 8D 90 <key> | 56 | 8B 35 <abs> | 05 <iv> ...
    drmp[block + 54] = '\x53';                                                   // push ebx
    drmp[block + 55] = '\x89'; drmp[block + 56] = '\x15';                        // mov [abs], edx
    drmp[block + 61] = '\x8D'; drmp[block + 62] = '\x90'; Put(&drmp, block + 63, 0x10, 4);  // lea edx, [eax+key]
    drmp[block + 67] = '\x56';                                                   // push esi
    drmp[block + 68] = '\x8B'; drmp[block + 69] = '\x35'; Put(&drmp, block + 70, 0x1005c018u, 4);  // mov esi, [abs]
    drmp[block + 74] = '\x05'; Put(&drmp, block + 75, 0x30, 4);                  // add eax, iv
  }
  std::uint32_t keys[4];
  for (int i = 0; i < 4; ++i) keys[i] = Get32(payload, 0x60 + 4 * i);
  std::uint32_t chain1 = 0x55555555, chain2 = 0x55555555;
  for (std::size_t at = 0; at < drmp.size(); at += 8) {
    std::uint32_t v0 = Get32(drmp, at) ^ chain1, v1 = Get32(drmp, at + 4) ^ chain2, sum = 0;
    for (int round = 0; round < 32; ++round) {
      v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + keys[sum & 3]);
      sum += 0x9E3779B9u;
      v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + keys[(sum >> 11) & 3]);
    }
    Put(&drmp, at, v0, 4);
    Put(&drmp, at + 4, v1, 4);
    chain1 = v0;
    chain2 = v1;
  }
  bytes.replace(drmp_at, drmp_size, drmp);

  std::uint32_t xor_key = 0x2468ace1;
  const std::uint32_t site_key = xor_key;
  Xor21(&header, 0, header.size(), &xor_key);
  Xor21(&payload, 0, payload.size(), &xor_key);
  bytes.replace(header_at, header.size(), header);
  bytes.replace(payload_at, payload_size, payload);

  const std::size_t entry_file = kBindRaw + (kBind21Entry - 0x3000);
  Put(&bytes, entry_file - 4, 0xC0DEC0DE, 4);
  const unsigned char start[] = {0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00,
                                 0xC7, 0x45, 0x80, 0, 0, 0, 0,             // mov [ebp-0x80], header
                                 0x8B, 0x75, 0x80,                         // mov esi, [ebp-0x80]
                                 0xB9, 0, 0, 0, 0,                         // mov ecx, dwords
                                 0x8D, 0xBD, 0x78, 0xFC, 0xFF, 0xFF,       // lea edi, [ebp-0x388]
                                 0xF3, 0xA5,                               // rep movsd
                                 0xC7, 0x85, 0x3C, 0xFC, 0xFF, 0xFF, 0, 0, 0, 0,  // mov [ebp-0x3c4], key
                                 0x8B, 0x85, 0x3C, 0xFC, 0xFF, 0xFF};
  bytes.replace(entry_file, sizeof(start), reinterpret_cast<const char*>(start), sizeof(start));
  Put(&bytes, entry_file + 17, kImage21 + 0x3000, 4);
  Put(&bytes, entry_file + 25, header_size_used / 4, 4);
  Put(&bytes, entry_file + 43, site_key, 4);
  return bytes;
}

// A 32-bit program wrapped like SteamStub 2.0 (header named by "mov esi"/"mov ecx" after the prologue,
// XOR-chained; code XOR-chained when flag 0x4 is set) or 1.0 (byte-XORed header of size10 bytes, entry in a jmp).
inline std::string BuildOld(bool v10, std::uint32_t flags20 = 0x04, std::size_t size20 = 884, std::size_t size10 = 0x6A * 4) {
  Spec base;
  base.pe32 = true;
  std::string bytes = Build(base);
  bytes.resize(kBindRaw + kBind21Size, '\0');
  std::fill(bytes.begin() + kBindRaw, bytes.end(), '\0');
  bytes.replace(kCodeRaw, kCodeSize, PlainCode());
  Put(&bytes, kTable + 2 * 40 + 8, kBind21Size, 4);
  Put(&bytes, kTable + 2 * 40 + 16, kBind21Size, 4);
  Put(&bytes, kOptional + 20, 0x1000, 4);  // BaseOfCode
  Put(&bytes, kOptional + 56, 0x4000 + kBind21Size - 0x1000, 4);
  constexpr std::uint32_t entry = 0x3C00;
  Put(&bytes, kOptional + 16, entry, 4);
  const std::size_t entry_file = kBindRaw + (entry - 0x3000);
  if (v10) {
    const std::size_t shift = size10 == 0x6A * 4 ? 0 : 4;
    std::string header(size10, '\0');
    Put(&header, 8 + shift, kImage21 + entry, 4);
    Put(&header, 20 + shift, 1500, 4);
    for (std::size_t i = 0; i < size10; ++i) header[i] = static_cast<char>(header[i] ^ static_cast<char>(i * i));
    bytes.replace(kBindRaw, size10, header);
    const unsigned char start[] = {0x60, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00, 0xBE, 0, 0, 0, 0, 0xB9, 0, 0, 0, 0};
    bytes.replace(entry_file, sizeof(start), reinterpret_cast<const char*>(start), sizeof(start));
    Put(&bytes, entry_file + 8, kImage21 + 0x3000, 4);
    Put(&bytes, entry_file + 13, size10 / 4, 4);
    const unsigned char jump[] = {0x61, 0xB8, 0, 0, 0, 0, 0xFF, 0xE0};
    bytes.replace(entry_file + 0x80, sizeof(jump), reinterpret_cast<const char*>(jump), sizeof(jump));
    Put(&bytes, entry_file + 0x82, kImage21 + kOriginalEntry, 4);
    return bytes;
  }
  constexpr std::uint32_t code_key = 0x0badf00d;
  if (flags20 & 0x04) {
    std::uint32_t previous = code_key;
    for (std::size_t at = 0; at < kCodeSize; at += 4) {
      const std::uint32_t stored = Get32(bytes, kCodeRaw + at) ^ previous;
      Put(&bytes, kCodeRaw + at, stored, 4);
      previous = stored;
    }
  }
  const std::size_t shift = size20 == 856 ? 0 : 4;
  std::string header(size20, '\0');
  Put(&header, 0, 0x5eed1234, 4);
  Put(&header, 20 + shift, flags20, 4);
  Put(&header, 40 + shift, kImage21 + kOriginalEntry, 4);
  Put(&header, 44 + shift, kImage21 + 0x1000, 4);
  Put(&header, 48 + shift, kCodeSize, 4);
  Put(&header, 52 + shift, code_key, 4);
  Put(&header, 56 + shift, 2000, 4);
  for (std::size_t at = 4; at < header.size(); at += 4) Put(&header, at, Get32(header, at) ^ Get32(header, at - 4), 4);
  bytes.replace(kBindRaw, size20, header);
  Put(&bytes, entry_file - 4, 0xC0DEC0DE, 4);
  const unsigned char start[] = {0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00,
                                 0xBE, 0, 0, 0, 0, 0xB9, 0, 0, 0, 0, 0xF3, 0xA5};
  bytes.replace(entry_file, sizeof(start), reinterpret_cast<const char*>(start), sizeof(start));
  Put(&bytes, entry_file + 15, kImage21 + 0x3000, 4);
  Put(&bytes, entry_file + 20, size20 / 4, 4);
  return bytes;
}

}  // namespace steam_stub_fixture
