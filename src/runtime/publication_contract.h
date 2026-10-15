#pragma once

#include "pw_title_publication.h"

#include <optional>
#include <string>

namespace playden::runtime {

// Canonical writers for the pinned wrapper's portable contracts. Every result
// passes the same strict C parser used by native admission before it is returned.
// These functions do not grant ownership or publish files; the transaction
// adapter binds expected paths/identities and supplies durability and locking.
std::optional<std::string> SerializePublicationReceipt(const PwTitlePublication& receipt);
std::optional<std::string> SerializePublicationMetadata(const PwTitlePublicationMetadata& metadata);

}  // namespace playden::runtime
