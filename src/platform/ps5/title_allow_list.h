#pragma once
// title_allow_list.h - The root-owned list of game titles an any-title kit's helper serves.
// Copyright (c) 2026 Z3CloudPlay contributors
// SPDX-License-Identifier: MIT
#include <filesystem>
#include <functional>
#include <string>
#include <sys/types.h>

namespace z3cloudplay::ps5 {
// Root is reached through Z3CloudPlay's own grant: it keeps a real UID of 0, so it can raise its
// effective UID. The effective UID is process-wide and other threads keep running, so root is
// raised only around the calls that create or rename root-owned entries.
struct RootFileOps {
  std::function<bool()> raise;   // become root; false when it can't
  std::function<void()> lower;   // back to the app user
  uid_t owner = 0;               // who the directory and the file must belong to
};

// Replaces directory/name with contents: the directory (0755) and the file (0644, one link)
// belong to ops.owner and nobody else can write them, so the Lapy helper's title rule accepts
// them. The new file is written beside the old one, synced, then renamed over it. Worker only.
bool WriteRootOwnedFile(const std::filesystem::path& directory, const std::string& name,
                        const std::string& contents, const RootFileOps& ops, std::string* error);

// runtime::kTitleRuleAllowList on the console, as root.
bool WriteTitleAllowList(const std::string& contents, std::string* error);
}  // namespace z3cloudplay::ps5
