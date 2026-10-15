#pragma once
#include "library/repository.h"
#include <string>

namespace z3cloudplay {
class LibraryCache final : public LibraryRepository {
public:
    explicit LibraryCache(Storage& storage) : storage_(storage) {}
    Result<LibrarySnapshot> Load(std::uint64_t account_id) const override;
    Result<void> Save(std::uint64_t account_id, const LibrarySnapshot& snapshot) const override;
    Result<void> Clear(std::uint64_t account_id) const override;
    // Kept for existing callers that already hold the canonical decimal ID.
    Result<LibrarySnapshot> Load(const std::string& account_id) const;
    Result<void> Save(const std::string& account_id, const LibrarySnapshot& snapshot) const;
    Result<void> Clear(const std::string& account_id) const;
private:
    Storage& storage_;
};
}
