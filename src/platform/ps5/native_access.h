#pragma once

#include <string>

namespace z3cloudplay::ps5 {

enum class NativePathKind {
  kMissing,
  kFile,
  kDirectory,
  kOther,
};

enum class NativePathAccess {
  kRead,
  kReadWrite,
};

// Native calls stay outside this portable bootstrap. The production adapter may
// use package metadata or a durable marker for source_identity; callers must not
// assume that two mount views expose matching device/inode values.
struct NativeAccessOps {
  void* context = nullptr;
  int (*initialize_user_service)(void*) = nullptr;
  int (*prepare_graphics_access)(void*) = nullptr;
  int (*request_data_access)(void*) = nullptr;
  NativePathKind (*path_kind)(void*, const char*) = nullptr;
  bool (*path_accessible)(void*, const char*, NativePathAccess) = nullptr;
  bool (*read_source_identity)(void*, const char*, std::string*) = nullptr;
};

struct NativeAccessOptions {
  std::string title_id;
  std::string sandbox_root = "/app0";
  std::string stable_homebrew_root = "/data/homebrew";
  std::string managed_runtime_root = "/data/z3cloudplay";
  std::string managed_runtime_parent = "/data";
  // Set only when durable install metadata proves this launcher has no prior
  // account/log state. Directory creation belongs to the owning services.
  bool known_fresh_state = false;
  // Optional durable package identity. When empty, it is captured from the
  // sandbox root before the grant can invalidate that mount view.
  std::string source_identity;
  // Helpers requiring a single-threaded target must run before UserService,
  // which may start background threads. Initialize it before returning to UI.
  bool user_service_after_grant = false;
  // Visible paths do not prove the grant ran: ShadowMountPlus 1.7beta4+
  // mounts /data into the sandbox, where the private trees stay closed.
  bool require_grant = false;
};

struct NativePaths {
  std::string title_root;
  std::string assets_root;
  std::string trust_store;
  std::string steam_state_root;
  std::string log_root;
  std::string runtime_data_root;
};

struct NativeAccessResult {
  bool launcher_ready = false;
  bool runtime_ready = false;
  bool granted = false;
  bool already_accessible = false;
  NativePaths paths;
  std::string error;
};

// Orders the portable parts of startup around the root-view change. Successful
// preparation is cached. A failed grant can be retried without repeating
// completed UserService/graphics preparation or mutating persistent storage.
class NativeAccessBootstrap {
 public:
  NativeAccessBootstrap(NativeAccessOps ops, NativeAccessOptions options);
  NativeAccessResult Prepare();

 private:
  NativeAccessResult PrepareAccess();
  NativeAccessResult LauncherFallback(const std::string& error,
                                      const NativePaths* stable = nullptr);

  NativeAccessOps ops_;
  NativeAccessOptions options_;
  bool user_initialized_ = false;
  bool graphics_prepared_ = false;
  bool source_identity_captured_ = false;
  std::string source_identity_;
  bool complete_ = false;
  NativeAccessResult complete_result_;
};

}  // namespace z3cloudplay::ps5
