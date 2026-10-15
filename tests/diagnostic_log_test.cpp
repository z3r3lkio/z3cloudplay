#include "app/diagnostic_log.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace z3cloudplay::app;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

int main() {
  CHECK(FormatLogTimestamp(0) == "1970-01-01T00:00:00Z");
  CHECK(FormatLogTimestamp(1'791'177'123) == "2026-10-05T05:12:03Z");
  CHECK(FormatLogTimestamp(951'782'400) == "2000-02-29T00:00:00Z");  // leap day

  const auto directory = std::filesystem::temp_directory_path() / ("z3cloudplay-log-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);

  // Missing file: not found, not an error.
  auto tail = ReadLogTail((directory / "install.log").string());
  CHECK(!tail.found && !tail.read_error && tail.lines.empty());

  // Appending creates the folder and stamps each line.
  CHECK(AppendLogLine(directory.string(), "install.log", "app=220 step=install result=none detail="));
  CHECK(AppendLogLine(directory.string(), "install.log", "app=140 step=trace result=write detail="));
  tail = ReadLogTail((directory / "install.log").string());
  CHECK(tail.found && !tail.truncated && tail.lines.size() == 2);
  CHECK(tail.lines[0].size() > 21 && tail.lines[0][4] == '-' && tail.lines[0][19] == 'Z');
  CHECK(tail.lines[0].substr(21) == "app=220 step=install result=none detail=");

  // A full file moves aside to ".1" before the next line.
  {
    std::ofstream big(directory / "auth.log");
    big << std::string(2'000, 'x') << '\n';
  }
  CHECK(AppendLogLine(directory.string(), "auth.log", "auth_failure method=X http=200 eresult=15", 1'000));
  CHECK(std::filesystem::exists(directory / "auth.log.1"));
  tail = ReadLogTail((directory / "auth.log").string());
  CHECK(tail.lines.size() == 1 && tail.lines[0].find("eresult=15") != std::string::npos);

  // Only the end of a large log is read; the partial first line is dropped; lines are cleaned and cut.
  {
    std::ofstream log(directory / "launch.log");
    for (int index = 0; index < 1'000; ++index) log << "line " << index << '\n';
    log << "tab\there\x01" << '\n';
    log << std::string(400, 'y') << "\r\n";
  }
  tail = ReadLogTail((directory / "launch.log").string(), 1'024, 10);
  CHECK(tail.found && tail.truncated && tail.lines.size() >= 2 && tail.lines.size() <= 10);
  CHECK(tail.lines[tail.lines.size() - 2] == "tab here?");
  CHECK(tail.lines.back().rfind(std::string(kMaxLogLineChars, 'y'), 0) == 0 && tail.lines.back().size() < 300);
  tail = ReadLogTail((directory / "launch.log").string());
  CHECK(tail.lines.size() == kLogTailLines && tail.truncated && tail.lines.front().rfind("line ", 0) == 0);
  // A window smaller than the last line yields no lines rather than a fragment.
  tail = ReadLogTail((directory / "launch.log").string(), 256, 10);
  CHECK(tail.found && tail.truncated && tail.lines.empty());

  std::filesystem::remove_all(directory);
  std::puts("diagnostic log tests passed");
}
