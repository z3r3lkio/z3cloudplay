#include "steam/backend/app_info.h"

namespace z3cloudplay::steam {
namespace {
bool IsControl(std::string_view s, std::size_t i) {
  const auto c = static_cast<unsigned char>(s[i]);
  if (c < 0x20 || c == 0x7F) return true;
  // U+0080..U+009F encode as C2 80..C2 9F.
  return c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
         static_cast<unsigned char>(s[i + 1]) <= 0x9F;
}

std::string LowerPath(std::string_view text) {
  std::string result(text);
  for (char& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return result;
}
}  // namespace

std::optional<std::string> RelativeGamePath(std::string_view raw, bool allow_root) {
  std::string path(raw);
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  if (!path.empty() && path.front() == '/') return std::nullopt;
  if (path.find(':') != std::string::npos) return std::nullopt;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (IsControl(path, i)) return std::nullopt;
  }
  std::vector<std::string_view> parts;
  std::string_view rest(path);
  while (!rest.empty()) {
    const std::size_t slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    if (!part.empty() && part != ".") parts.push_back(part);
    if (slash == std::string_view::npos) break;
    rest.remove_prefix(slash + 1);
  }
  for (const std::string_view part : parts) {
    if (part == "..") return std::nullopt;
  }
  if (!parts.empty() && LowerPath(parts.front()) == ".gn-download") return std::nullopt;
  if (parts.empty()) {
    if (!allow_root) return std::nullopt;
    return std::string(".");
  }
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.push_back('/');
    out.append(parts[i]);
  }
  return out;
}

}  // namespace z3cloudplay::steam
