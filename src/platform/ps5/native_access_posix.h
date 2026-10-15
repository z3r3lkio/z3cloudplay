#pragma once

#include "platform/ps5/native_access.h"

namespace z3cloudplay::ps5 {

struct NativeAccessPreparation {
  void* context = nullptr;
  int (*initialize_user_service)(void*) = nullptr;
  int (*prepare_graphics_access)(void*) = nullptr;
  int (*request_data_access)(void*) = nullptr;
};

// Filesystem adapter for both the native title and host integration tests.
// The package identity is authored after signing and compared across mount views.
// It distinguishes packages; it is not an authentication boundary for local files.
class NativeAccessPosixBackend {
 public:
  NativeAccessPosixBackend(std::string title_id, NativeAccessPreparation preparation);
  NativeAccessOps Ops();

 private:
  static int Initialize(void*);
  static int PrepareGraphics(void*);
  static int RequestData(void*);
  static NativePathKind Kind(void*, const char*);
  static bool Accessible(void*, const char*, NativePathAccess);
  static bool Identity(void*, const char*, std::string*);
  std::string title_id_;
  NativeAccessPreparation preparation_;
};

}  // namespace z3cloudplay::ps5
