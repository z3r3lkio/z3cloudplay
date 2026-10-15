#pragma once

#include "library/model.h"
#include "platform/storage.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace z3cloudplay {

enum class CompatibilityRating : std::uint8_t {
  kUntested = 0,
  kWorks = 1,
  kPlayable = 2,
  kBroken = 3,
};

struct OrganizationCollection {
  std::string id;
  std::string name;
  bool pinned = false;
  std::set<GameId> members;
};

struct OrganizationSnapshot {
  std::uint16_t schema_version = 1;
  std::set<GameId> favorites;
  std::set<GameId> hidden;
  std::vector<OrganizationCollection> collections;
  std::map<GameId, CompatibilityRating> ratings;
  std::map<GameId, std::string> notes;
  std::string query;
  std::string scope = "all";
  std::string sort = "name";
  std::set<std::string> filters;
  GameId selected_game_id = 0;
};

bool operator==(const OrganizationCollection& left,
                const OrganizationCollection& right);
bool operator==(const OrganizationSnapshot& left,
                const OrganizationSnapshot& right);

// Pure validation shared by persistence and presentation, including encoded size.
bool OrganizationSnapshotIsValid(const OrganizationSnapshot& snapshot);

class OrganizationRepository {
 public:
  virtual ~OrganizationRepository() = default;
  virtual Result<std::optional<OrganizationSnapshot>> Load(
      std::uint64_t account_id) const = 0;
  virtual Result<void> Save(std::uint64_t account_id,
                            const OrganizationSnapshot& snapshot) const = 0;
  virtual Result<void> Clear(std::uint64_t account_id) const = 0;
};

class OrganizationStore final : public OrganizationRepository {
 public:
  explicit OrganizationStore(Storage& storage) : storage_(storage) {}
  Result<std::optional<OrganizationSnapshot>> Load(
      std::uint64_t account_id) const override;
  Result<void> Save(std::uint64_t account_id,
                    const OrganizationSnapshot& snapshot) const override;
  Result<void> Clear(std::uint64_t account_id) const override;

 private:
  Storage& storage_;
};

}  // namespace z3cloudplay
