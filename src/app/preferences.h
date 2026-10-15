#pragma once

#include "platform/storage.h"

#include <string>

namespace z3cloudplay {

struct Preferences {
  bool reduce_motion = false;
  // PS5 titles made for games are named "<game> (<store>)", e.g. "Half-Life 2 (Steam)".
  bool title_store_suffix = false;
  // Cloud saves move on their own: before Play, after a session, at startup and before an
  // uninstall. Off (the default, so a fresh install never changes Cloud saves unasked) leaves only
  // the Sync actions the player chooses.
  bool cloud_saves_auto = false;
};

enum class PreferencesLoadStatus {
  kLoaded,
  kDefaultMissing,
  kDefaultCorrupt,
  kDefaultReadError,
};

struct PreferencesLoadResult {
  Preferences preferences;
  PreferencesLoadStatus status = PreferencesLoadStatus::kLoaded;
  std::string diagnostic;
};

class PreferencesStore {
 public:
  explicit PreferencesStore(Storage& storage);

  PreferencesLoadResult Load() const;
  Result<void> Save(const Preferences& preferences) const;

 private:
  Storage& storage_;
};

}  // namespace z3cloudplay
