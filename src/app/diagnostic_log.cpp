#include "app/diagnostic_log.h"

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdio>

namespace z3cloudplay::app {
namespace {

// Howard Hinnant's civil_from_days: days since 1970-01-01 to year/month/day.
void CivilFromDays(std::int64_t days, std::int64_t* year, unsigned* month, unsigned* day) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned month_index = (5 * day_of_year + 2) / 153;
  *day = day_of_year - (153 * month_index + 2) / 5 + 1;
  *month = month_index < 10 ? month_index + 3 : month_index - 9;
  *year = static_cast<std::int64_t>(year_of_era) + era * 400 + (*month <= 2 ? 1 : 0);
}

std::string Sanitize(std::string line) {
  for (auto& value : line) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte == '\t') value = ' ';
    else if (byte < 0x20 || byte == 0x7f) value = '?';
  }
  if (line.size() > kMaxLogLineChars) {
    std::size_t cut = kMaxLogLineChars;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) --cut;  // keep UTF-8 whole
    line.resize(cut);
    line += "…";
  }
  return line;
}

}  // namespace

std::string FormatLogTimestamp(std::int64_t unix_seconds) {
  const std::int64_t days = (unix_seconds >= 0 ? unix_seconds : unix_seconds - 86399) / 86400;
  const std::int64_t seconds = unix_seconds - days * 86400;
  std::int64_t year = 0;
  unsigned month = 0, day = 0;
  CivilFromDays(days, &year, &month, &day);
  char text[32];
  std::snprintf(text, sizeof(text), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", static_cast<long long>(year), month, day,
                static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60),
                static_cast<long long>(seconds % 60));
  return text;
}

bool AppendLogLine(const std::string& directory, const std::string& file, const std::string& line, std::size_t max_bytes) {
  mkdir(directory.c_str(), 0777);
  const auto path = directory + "/" + file;
  struct stat info {};
  if (stat(path.c_str(), &info) == 0 && static_cast<std::size_t>(info.st_size) > max_bytes) {
    const auto previous = path + ".1";
    std::remove(previous.c_str());
    std::rename(path.c_str(), previous.c_str());
  }
  FILE* log = std::fopen(path.c_str(), "a");
  if (!log) return false;
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
  const bool written = std::fprintf(log, "%s %s\n", FormatLogTimestamp(now).c_str(), line.c_str()) > 0;
  return std::fclose(log) == 0 && written;
}

LogTail ReadLogTail(const std::string& path, std::size_t max_bytes, std::size_t max_lines) {
  LogTail tail;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    tail.read_error = errno != ENOENT;
    tail.found = tail.read_error;
    return tail;
  }
  tail.found = true;
  long size = 0;
  if (std::fseek(file, 0, SEEK_END) != 0 || (size = std::ftell(file)) < 0) {
    std::fclose(file);
    tail.read_error = true;
    return tail;
  }
  const auto start = static_cast<std::size_t>(size) > max_bytes ? static_cast<std::size_t>(size) - max_bytes : 0;
  std::string text(static_cast<std::size_t>(size) - start, '\0');
  const bool read = std::fseek(file, static_cast<long>(start), SEEK_SET) == 0 &&
                    std::fread(text.data(), 1, text.size(), file) == text.size();
  std::fclose(file);
  if (!read) {
    tail.read_error = true;
    return tail;
  }
  std::size_t begin = 0;
  if (start > 0) {
    // The first line was cut by the byte window.
    tail.truncated = true;
    const auto newline = text.find('\n');
    begin = newline == std::string::npos ? text.size() : newline + 1;
  }
  while (begin < text.size()) {
    auto end = text.find('\n', begin);
    if (end == std::string::npos) end = text.size();
    auto line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) tail.lines.push_back(Sanitize(std::move(line)));
    begin = end + 1;
  }
  if (tail.lines.size() > max_lines) {
    tail.lines.erase(tail.lines.begin(), tail.lines.end() - static_cast<std::ptrdiff_t>(max_lines));
    tail.truncated = true;
  }
  return tail;
}

}  // namespace z3cloudplay::app
