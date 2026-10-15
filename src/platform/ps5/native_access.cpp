#include "platform/ps5/native_access.h"

#include <cctype>
#include <utility>

namespace z3cloudplay::ps5 {
namespace {

std::string Join(const std::string& root, const std::string& child) {
  if (root.empty()) return {};
  if (root.back() == '/') return root + child;
  return root + "/" + child;
}

bool ValidTitleId(const std::string& title_id) {
  if (title_id.size() != 9) return false;
  for (std::size_t index = 0; index < title_id.size(); ++index) {
    const unsigned char value = static_cast<unsigned char>(title_id[index]);
    if (index < 4 ? !std::isupper(value) : !std::isdigit(value)) return false;
  }
  return true;
}

NativePaths PathsAt(const std::string& root) {
  return {root, Join(root, "assets"), Join(root, "trust/cacert.pem"),
          Join(root, "z3cloudplay-data"), Join(root, "logs"), {}};
}

bool ValidatePaths(const NativeAccessOps& ops, const NativePaths& paths,
                   bool known_fresh_state, std::string* error) {
  if (ops.path_kind(ops.context, paths.title_root.c_str()) !=
          NativePathKind::kDirectory ||
      !ops.path_accessible(ops.context, paths.title_root.c_str(),
                           NativePathAccess::kRead)) {
    *error = "title root is unavailable";
    return false;
  }
  if (ops.path_kind(ops.context, paths.assets_root.c_str()) !=
          NativePathKind::kDirectory ||
      !ops.path_accessible(ops.context, paths.assets_root.c_str(),
                           NativePathAccess::kRead)) {
    *error = "assets root is unavailable";
    return false;
  }
  if (ops.path_kind(ops.context, paths.trust_store.c_str()) !=
          NativePathKind::kFile ||
      !ops.path_accessible(ops.context, paths.trust_store.c_str(),
                           NativePathAccess::kRead)) {
    *error = "trust store is unavailable";
    return false;
  }
  const auto state = ops.path_kind(ops.context, paths.steam_state_root.c_str());
  if (state != NativePathKind::kDirectory &&
      !(known_fresh_state && state == NativePathKind::kMissing)) {
    *error = "existing Steam state root is unavailable";
    return false;
  }
  if (state == NativePathKind::kDirectory &&
      !ops.path_accessible(ops.context, paths.steam_state_root.c_str(),
                           NativePathAccess::kReadWrite)) {
    *error = "existing Steam state root is inaccessible";
    return false;
  }
  const auto logs = ops.path_kind(ops.context, paths.log_root.c_str());
  if (logs != NativePathKind::kDirectory &&
      !(known_fresh_state && logs == NativePathKind::kMissing)) {
    *error = "existing log root is unavailable";
    return false;
  }
  if (logs == NativePathKind::kDirectory &&
      !ops.path_accessible(ops.context, paths.log_root.c_str(),
                           NativePathAccess::kReadWrite)) {
    *error = "existing log root is inaccessible";
    return false;
  }
  if ((state == NativePathKind::kMissing || logs == NativePathKind::kMissing) &&
      !ops.path_accessible(ops.context, paths.title_root.c_str(),
                           NativePathAccess::kReadWrite)) {
    *error = "fresh Steam state or log root cannot be created";
    return false;
  }
  return true;
}

bool ValidateRuntimeData(const NativeAccessOps& ops,
                         const NativeAccessOptions& options,
                         std::string* error) {
  const auto root =
      ops.path_kind(ops.context, options.managed_runtime_root.c_str());
  if (root == NativePathKind::kDirectory) {
    if (ops.path_accessible(ops.context, options.managed_runtime_root.c_str(),
                            NativePathAccess::kReadWrite))
      return true;
    *error = "managed runtime data root is inaccessible";
    return false;
  }
  if (root != NativePathKind::kMissing) {
    *error = "managed runtime data root is not a directory";
    return false;
  }
  if (ops.path_kind(ops.context, options.managed_runtime_parent.c_str()) ==
          NativePathKind::kDirectory &&
      ops.path_accessible(ops.context, options.managed_runtime_parent.c_str(),
                          NativePathAccess::kReadWrite))
    return true;
  *error = "managed runtime data root cannot be created";
  return false;
}

std::string NativeFailure(const char* operation, int code) {
  return std::string(operation) + " failed (code " + std::to_string(code) + ")";
}

}  // namespace

NativeAccessBootstrap::NativeAccessBootstrap(NativeAccessOps ops,
                                             NativeAccessOptions options)
    : ops_(ops), options_(std::move(options)) {}

NativeAccessResult NativeAccessBootstrap::LauncherFallback(
    const std::string& error, const NativePaths* stable) {
  NativeAccessResult result;
  result.error = error;
  if (!ops_.path_kind || !ops_.path_accessible) return result;

  if (stable && source_identity_captured_ && ops_.read_source_identity) {
    std::string path_error;
    std::string identity;
    if (ValidatePaths(ops_, *stable, options_.known_fresh_state, &path_error) &&
        ops_.read_source_identity(ops_.context, stable->title_root.c_str(),
                                  &identity) &&
        !identity.empty() && identity == source_identity_) {
      result.launcher_ready = true;
      result.paths = *stable;
      result.paths.runtime_data_root.clear();
      return result;
    }
  }

  const NativePaths sandbox = PathsAt(options_.sandbox_root);
  std::string path_error;
  if (!ValidatePaths(ops_, sandbox, options_.known_fresh_state, &path_error)) {
    result.error += "; launcher paths unavailable: " + path_error;
    return result;
  }
  result.launcher_ready = true;
  result.paths = sandbox;
  return result;
}

NativeAccessResult NativeAccessBootstrap::Prepare() {
  if (complete_) return complete_result_;
  auto result = PrepareAccess();
  if (options_.user_service_after_grant && result.launcher_ready &&
      !user_initialized_ && ops_.initialize_user_service) {
    const int code = ops_.initialize_user_service(ops_.context);
    if (code != 0) {
      complete_ = false;
      result.runtime_ready = false;
      if (!result.error.empty()) result.error += "; ";
      result.error += NativeFailure("user service initialization", code);
      return result;
    }
    user_initialized_ = true;
  }
  if (complete_) complete_result_ = result;
  return result;
}

NativeAccessResult NativeAccessBootstrap::PrepareAccess() {
  if (complete_) return complete_result_;
  if (!ValidTitleId(options_.title_id) || options_.sandbox_root.empty() ||
      options_.stable_homebrew_root.empty() ||
      options_.managed_runtime_root.empty() ||
      options_.managed_runtime_parent.empty()) {
    return LauncherFallback("native access configuration is invalid");
  }
  if (!ops_.initialize_user_service || !ops_.prepare_graphics_access ||
      !ops_.request_data_access || !ops_.path_kind ||
      !ops_.path_accessible ||
      !ops_.read_source_identity) {
    return LauncherFallback("native access operations are unavailable");
  }

  if (!user_initialized_ && !options_.user_service_after_grant) {
    const int code = ops_.initialize_user_service(ops_.context);
    if (code != 0)
      return LauncherFallback(NativeFailure("user service initialization", code));
    user_initialized_ = true;
  }
  if (!graphics_prepared_) {
    const int code = ops_.prepare_graphics_access(ops_.context);
    if (code != 0)
      return LauncherFallback(NativeFailure("graphics/module preparation", code));
    graphics_prepared_ = true;
  }

  if (!source_identity_captured_) {
    if (!options_.source_identity.empty()) {
      source_identity_ = options_.source_identity;
    } else {
      std::string sandbox_error;
      if (!ValidatePaths(ops_, PathsAt(options_.sandbox_root),
                         options_.known_fresh_state, &sandbox_error)) {
        return LauncherFallback("launcher source is invalid: " + sandbox_error);
      }
      if (!ops_.read_source_identity(ops_.context,
                                     options_.sandbox_root.c_str(),
                                     &source_identity_) ||
          source_identity_.empty()) {
        return LauncherFallback("launcher source identity is unavailable");
      }
    }
    source_identity_captured_ = true;
  }

  const std::string stable_root =
      Join(options_.stable_homebrew_root, options_.title_id);
  const NativePaths stable = PathsAt(stable_root);
  auto stable_title_ready = [&]() {
    std::string ignored;
    if (!ValidatePaths(ops_, stable, options_.known_fresh_state, &ignored))
      return false;
    std::string identity;
    return ops_.read_source_identity(ops_.context, stable_root.c_str(),
                                     &identity) &&
           !identity.empty() && identity == source_identity_;
  };

  std::string runtime_error;
  if (!options_.require_grant && stable_title_ready() &&
      ValidateRuntimeData(ops_, options_, &runtime_error)) {
    NativeAccessResult result;
    result.launcher_ready = true;
    result.runtime_ready = true;
    result.granted = true;
    result.already_accessible = true;
    result.paths = stable;
    result.paths.runtime_data_root = options_.managed_runtime_root;
    complete_result_ = result;
    complete_ = true;
    return result;
  }

  const int grant_code = ops_.request_data_access(ops_.context);
  if (grant_code != 0)
    return LauncherFallback(NativeFailure("native data grant", grant_code),
                            &stable);

  std::string path_error;
  if (!ValidatePaths(ops_, stable, options_.known_fresh_state, &path_error)) {
    return LauncherFallback("stable launcher source is invalid: " + path_error,
                            &stable);
  }

  std::string stable_identity;
  if (!ops_.read_source_identity(ops_.context, stable_root.c_str(),
                                 &stable_identity) ||
      stable_identity.empty() || stable_identity != source_identity_) {
    return LauncherFallback("stable launcher source identity does not match",
                            &stable);
  }

  runtime_error.clear();
  if (!ValidateRuntimeData(ops_, options_, &runtime_error))
    return LauncherFallback(runtime_error, &stable);

  NativeAccessResult result;
  result.launcher_ready = true;
  result.runtime_ready = true;
  result.granted = true;
  result.paths = stable;
  result.paths.runtime_data_root = options_.managed_runtime_root;
  complete_result_ = result;
  complete_ = true;
  return result;
}

}  // namespace z3cloudplay::ps5
