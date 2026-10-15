#include "app/preferences.h"

#include <string>

namespace z3cloudplay {
namespace {

constexpr char kPreferencesPath[] = "preferences.conf";

std::string ToString(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}


}  // namespace

PreferencesStore::PreferencesStore(Storage& storage) : storage_(storage) {}

PreferencesLoadResult PreferencesStore::Load() const {
  const auto ready=storage_.EnsureRootDirectory();
  if(!ready.ok()) return {Preferences{},PreferencesLoadStatus::kDefaultReadError,"Preference storage is unavailable."};
  const auto read = storage_.Read(kPreferencesPath);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) {
      return PreferencesLoadResult{Preferences{},
                                   PreferencesLoadStatus::kDefaultMissing,
                                   std::string()};
    }
    return PreferencesLoadResult{
        Preferences{}, PreferencesLoadStatus::kDefaultReadError,
        "Preferences could not be read; defaults restored: " + read.message()};
  }

  const std::string serialized = ToString(read.value());
  // Version 1 holds only reduce_motion; version 2 adds title_store_suffix; version 3 cloud_saves_auto.
  for (const int motion : {0, 1}) {
    if (serialized == std::string("version=1\nreduce_motion=") + char('0' + motion) + "\n") {
      Preferences preferences;
      preferences.reduce_motion = motion == 1;
      return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
    }
    for (const int suffix : {0, 1})
      if (serialized == std::string("version=2\nreduce_motion=") + char('0' + motion) +
                            "\ntitle_store_suffix=" + char('0' + suffix) + "\n") {
        Preferences preferences;
        preferences.reduce_motion = motion == 1;
        preferences.title_store_suffix = suffix == 1;
        return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
      }
    for (const int suffix : {0, 1})
      for (const int cloud : {0, 1})
        if (serialized == std::string("version=3\nreduce_motion=") + char('0' + motion) +
                              "\ntitle_store_suffix=" + char('0' + suffix) + "\ncloud_saves_auto=" + char('0' + cloud) + "\n") {
          Preferences preferences;
          preferences.reduce_motion = motion == 1;
          preferences.title_store_suffix = suffix == 1;
          preferences.cloud_saves_auto = cloud == 1;
          return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
        }
  }
  return PreferencesLoadResult{
      Preferences{}, PreferencesLoadStatus::kDefaultCorrupt,
      "Preferences file is corrupt; defaults restored."};
}

Result<void> PreferencesStore::Save(const Preferences& preferences) const {
  const auto ready=storage_.EnsureRootDirectory();
  if(!ready.ok()) return ready;
  const std::string text = std::string("version=3\nreduce_motion=") + (preferences.reduce_motion ? "1" : "0") +
                           "\ntitle_store_suffix=" + (preferences.title_store_suffix ? "1" : "0") +
                           "\ncloud_saves_auto=" + (preferences.cloud_saves_auto ? "1" : "0") + "\n";
  return storage_.WriteAtomic(kPreferencesPath, Bytes(text.begin(), text.end()));
}

}  // namespace z3cloudplay
