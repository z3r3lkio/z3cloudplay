#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace z3cloudplay::app {

// Plain-text diagnostic logs (install, sign-in, launch) under one folder. Lines are sanitized by
// their writers: never tokens or account identifiers.
inline constexpr std::size_t kMaxLogFileBytes = 512 * 1024;
inline constexpr std::size_t kLogTailBytes = 128 * 1024;
inline constexpr std::size_t kLogTailLines = 400;
inline constexpr std::size_t kMaxLogLineChars = 240;

// "2026-10-05T07:12:03Z" for a Unix time, without relying on the platform's time zone support.
std::string FormatLogTimestamp(std::int64_t unix_seconds);

// Appends "<UTC timestamp> <line>" to directory/file, creating the folder. A file that has grown
// past max_bytes is first moved to "<file>.1", replacing the previous one.
bool AppendLogLine(const std::string& directory, const std::string& file, const std::string& line,
                   std::size_t max_bytes = kMaxLogFileBytes);

struct LogTail {
  bool found = false;       // the file exists
  bool read_error = false;  // it exists but could not be read
  bool truncated = false;   // older lines were left out
  std::vector<std::string> lines;  // oldest first, control characters replaced, long lines cut
};

// The newest lines of a log, reading at most max_bytes from its end.
LogTail ReadLogTail(const std::string& path, std::size_t max_bytes = kLogTailBytes,
                    std::size_t max_lines = kLogTailLines);

}  // namespace z3cloudplay::app
