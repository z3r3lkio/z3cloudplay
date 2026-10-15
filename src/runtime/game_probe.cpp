#include "runtime/game_probe.h"

#include "runtime/steam_stub.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string text) {
  for (auto& value : text)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  return text;
}

bool EndsWith(const std::string& text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A program file opened without following links. Reads are positioned, so the import
// table is found through the section table wherever it sits in a large program.
class Program {
 public:
  explicit Program(const std::string& path) : file_(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) {}
  ~Program() { if (file_ >= 0) ::close(file_); }
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  bool ReadAt(std::uint64_t offset, void* buffer, std::size_t size) const {
    if (file_ < 0) return false;
    auto* bytes = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < size) {
      const auto count = ::pread(file_, bytes + done, size - done, static_cast<off_t>(offset + done));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      done += static_cast<std::size_t>(count);
    }
    return true;
  }
  // Up to size bytes at offset; returns how many were read.
  std::size_t ReadSome(std::uint64_t offset, unsigned char* buffer, std::size_t size) const {
    if (file_ < 0) return 0;
    for (;;) {
      const auto count = ::pread(file_, buffer, size, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) continue;
      return count < 0 ? 0 : static_cast<std::size_t>(count);
    }
  }

 private:
  int file_;
};

std::uint32_t U32(const unsigned char* bytes) {
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
std::uint16_t U16(const unsigned char* bytes) { return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)); }

struct Section {
  std::string name;
  std::uint32_t address = 0;
  std::uint32_t size = 0;
  std::uint32_t raw = 0;
  std::uint32_t flags = 0;
};

// The console's memory pages are 16 KB; Windows lays sections out on 4 KB ones.
constexpr std::uint32_t kConsolePage = 0x4000;
constexpr std::uint32_t kSharedWritable = 0x10000000u | 0x80000000u;  // IMAGE_SCN_MEM_SHARED | _WRITE

struct PeFacts {
  bool valid = false;
  bool clr = false;
  std::string entry_section;
  // A writable section shared between processes that doesn't fill whole console pages.
  bool unaligned_shared = false;
  std::vector<std::string> imports;  // lower case
};

PeFacts ReadPe(const std::string& path) {
  PeFacts facts;
  Program program(path);
  unsigned char dos[64];
  if (!program.ReadAt(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return facts;
  const std::uint32_t pe = U32(dos + 0x3c);
  if (pe > 64u * 1024u * 1024u) return facts;
  unsigned char header[24];
  if (!program.ReadAt(pe, header, sizeof(header)) || std::memcmp(header, "PE\0\0", 4) != 0) return facts;
  const unsigned sections = U16(header + 6);
  const unsigned optional_size = U16(header + 20);
  if (sections == 0 || sections > 96 || optional_size < 96 || optional_size > 1024) return facts;
  std::vector<unsigned char> optional(optional_size);
  if (!program.ReadAt(pe + 24, optional.data(), optional.size())) return facts;
  const unsigned magic = U16(optional.data());
  if (magic != 0x10b && magic != 0x20b) return facts;
  const std::uint32_t entry = U32(optional.data() + 16);
  const std::size_t directories = magic == 0x20b ? 112 : 96;
  const auto directory = [&](unsigned index) -> std::uint32_t {
    const auto at = directories + index * 8;
    return at + 8 <= optional.size() ? U32(optional.data() + at) : 0;
  };
  const std::uint32_t import_rva = directory(1);
  facts.clr = directory(14) != 0;

  std::vector<unsigned char> table(sections * 40u);
  if (!program.ReadAt(pe + 24 + optional_size, table.data(), table.size())) return facts;
  std::vector<Section> list;
  for (unsigned i = 0; i < sections; ++i) {
    const auto* row = table.data() + i * 40;
    Section section;
    section.name.assign(reinterpret_cast<const char*>(row), strnlen(reinterpret_cast<const char*>(row), 8));
    section.address = U32(row + 12);
    section.size = std::max(U32(row + 8), U32(row + 16));
    section.raw = U32(row + 20);
    section.flags = U32(row + 36);
    const auto mapped = (U32(row + 8) + 0xfffu) & ~0xfffu;
    if ((section.flags & kSharedWritable) == kSharedWritable &&
        (section.address % kConsolePage != 0 || mapped % kConsolePage != 0))
      facts.unaligned_shared = true;
    list.push_back(section);
  }
  const auto file_offset = [&](std::uint32_t rva) -> std::optional<std::uint64_t> {
    for (const auto& section : list)
      if (rva >= section.address && rva - section.address < section.size)
        return static_cast<std::uint64_t>(section.raw) + (rva - section.address);
    return std::nullopt;
  };
  for (const auto& section : list)
    if (entry >= section.address && entry - section.address < section.size) facts.entry_section = section.name;
  facts.valid = true;

  auto descriptor = import_rva ? file_offset(import_rva) : std::nullopt;
  for (unsigned count = 0; descriptor && count < 512; ++count, *descriptor += 20) {
    unsigned char row[20];
    if (!program.ReadAt(*descriptor, row, sizeof(row))) break;
    const std::uint32_t name_rva = U32(row + 12);
    if (name_rva == 0) break;
    const auto name_at = file_offset(name_rva);
    if (!name_at) break;
    char name[128] = {};
    const auto got = program.ReadSome(*name_at, reinterpret_cast<unsigned char*>(name), sizeof(name) - 1);
    facts.imports.push_back(Lower(std::string(name, strnlen(name, got))));
  }
  return facts;
}

// Graphics library names a program loads at run time (Unity, GameMaker and others call
// LoadLibrary instead of importing). ASCII and UTF-16, any case.
const std::array<std::pair<const char*, const char*>, 9>& Libraries() {
  static const std::array<std::pair<const char*, const char*>, 9> list = {{
      {"d3d8.dll", "d3d8"}, {"d3d9.dll", "d3d9"}, {"d3d10.dll", "d3d10"}, {"d3d10_1.dll", "d3d10"},
      {"d3d11.dll", "d3d11"}, {"dxgi.dll", "dxgi"}, {"d3d12.dll", "d3d12"}, {"opengl32.dll", "opengl"},
      {"vulkan-1.dll", "vulkan"}}};
  return list;
}

void AddImports(const PeFacts& facts, std::set<std::string>* apis) {
  for (const auto& name : facts.imports) {
    for (const auto& [library, api] : Libraries())
      if (name == library) apis->insert(api);
    if (name == "ddraw.dll") apis->insert("ddraw");
  }
}

// Engines known only by a name inside the program: SIL (FTL) reads SIL_WINDOWS_* settings.
constexpr std::pair<const char*, const char*> kEngineMarks[] = {{"sil_windows_use_", "sil"}};

void ScanNames(const std::string& path, std::set<std::string>* apis, std::string* engine = nullptr) {
  Program program(path);
  std::vector<std::string> needles;
  std::vector<const char*> found_as;
  for (const auto& [library, api] : Libraries()) {
    needles.emplace_back(library);
    found_as.push_back(api);
    std::string wide;
    for (const char* at = library; *at; ++at) wide += *at, wide += '\0';
    needles.push_back(wide);
    found_as.push_back(api);
  }
  constexpr std::size_t kChunk = 1 << 20;
  constexpr std::size_t kOverlap = 32;
  std::vector<unsigned char> buffer(kChunk + kOverlap);  // heap: console worker stacks are small
  std::size_t kept = 0;
  for (std::uint64_t offset = 0; offset < kProbeScanBytes;) {
    const auto count = program.ReadSome(offset, buffer.data() + kept, kChunk);
    if (count == 0) break;
    const auto size = kept + count;
    for (std::size_t i = kept; i < size; ++i)
      if (buffer[i] >= 'A' && buffer[i] <= 'Z') buffer[i] = static_cast<unsigned char>(buffer[i] - 'A' + 'a');
    const std::string_view text(reinterpret_cast<const char*>(buffer.data()), size);
    for (std::size_t n = 0; n < needles.size(); ++n)
      if (text.find(needles[n]) != std::string_view::npos) apis->insert(found_as[n]);
    if (engine && engine->empty())
      for (const auto& [mark, name] : kEngineMarks)
        if (text.find(mark) != std::string_view::npos) *engine = name;
    offset += count;
    kept = std::min(kOverlap, size);
    std::memmove(buffer.data(), buffer.data() + size - kept, kept);
  }
}

// Entries of a folder, without following links: name as spelled -> is a directory.
std::vector<std::pair<std::string, bool>> Entries(const fs::path& folder) {
  std::vector<std::pair<std::string, bool>> out;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), last; !error && it != last && out.size() < 4096; it.increment(error)) {
    const auto status = it->symlink_status(error);
    if (error) { error.clear(); continue; }
    if (fs::is_symlink(status)) continue;
    out.emplace_back(it->path().filename().string(), fs::is_directory(status));
  }
  return out;
}

std::optional<std::string> Find(const std::vector<std::pair<std::string, bool>>& entries, std::string_view wanted,
                                bool directory) {
  for (const auto& [name, is_directory] : entries)
    if (is_directory == directory && Lower(name) == wanted) return name;
  return std::nullopt;
}

// The Unreal program under root: <Project>/Binaries/Win64/<Project>-Win64-Shipping.exe,
// searched a few levels deep. Returns the path relative to root, or empty.
std::string FindShipping(const fs::path& root, const std::string& relative, int depth) {
  if (depth > 4) return {};
  const auto entries = Entries(root / relative);
  for (const auto& [name, directory] : entries)
    if (!directory && EndsWith(Lower(name), "-shipping.exe") && Lower(relative).find("binaries/win64") != std::string::npos)
      return relative.empty() ? name : relative + "/" + name;
  for (const auto& [name, directory] : entries) {
    if (!directory) continue;
    const auto lower = Lower(name);
    if (lower == "engine" || lower == "content" || lower == "saved" || lower == "plugins") continue;
    const auto found = FindShipping(root, relative.empty() ? name : relative + "/" + name, depth + 1);
    if (!found.empty()) return found;
  }
  return {};
}

}  // namespace

GameProbe ProbeGame(const std::string& install_root, const std::string& executable) {
  GameProbe probe;
  const fs::path root(install_root);
  const auto slash = executable.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string() : executable.substr(0, slash);
  const std::string file = slash == std::string::npos ? executable : executable.substr(slash + 1);
  const auto lower_file = Lower(file);
  const std::string stem = EndsWith(lower_file, ".exe") ? file.substr(0, file.size() - 4) : file;
  const auto here = Entries(root / folder);
  const auto top = Entries(root);

  const auto program = ReadPe((root / executable).string());
  probe.dotnet = program.clr || std::count(program.imports.begin(), program.imports.end(), "mscoree.dll") > 0;
  probe.steam_stub = Lower(program.entry_section) == ".bind";
  if (probe.steam_stub) {
    const auto stub = InspectSteamStub((root / executable).string());
    probe.steam_stub_supported = stub.kind == SteamStubKind::kSupported;
    probe.steam_stub_detail = stub.detail;
  }
  probe.shared_section = program.unaligned_shared;
  AddImports(program, &probe.apis);
  for (const auto& [name, directory] : here)
    if (!directory && EndsWith(Lower(name), ".dll")) AddImports(ReadPe((root / folder / name).string()), &probe.apis);
  std::string marked_engine;
  ScanNames((root / executable).string(), &probe.apis, &marked_engine);

  if (const auto unity = Find(here, "unityplayer.dll", false); unity || Find(here, Lower(stem) + "_data", true)) {
    probe.engine = "unity";
    probe.il2cpp = Find(here, "gameassembly.dll", false).has_value();
    probe.legacy_unity = !unity;
    if (unity) ScanNames((root / folder / *unity).string(), &probe.apis);
  } else if (EndsWith(lower_file, "-shipping.exe") || Find(here, "engine", true)) {
    probe.engine = "unreal";
    if (!EndsWith(lower_file, "-shipping.exe")) probe.unreal_program = FindShipping(root, folder, 0);
  } else if (const auto bin = Find(here, "bin", true); bin && Find(Entries(root / folder / *bin), "engine.dll", false)) {
    probe.engine = "source";
  } else if (Find(here, "data.win", false)) {
    probe.engine = "gamemaker";
  } else if (Find(here, Lower(stem) + ".pck", false)) {
    probe.engine = "godot";
  } else if (Find(here, "love.dll", false)) {
    probe.engine = "love";  // LÖVE draws with OpenGL through SDL, which it loads at run time
  } else if (Find(here, "nw.dll", false)) {
    probe.engine = "nwjs";  // NW.js (RPG Maker MV and MZ): Chromium with Node
  } else if (std::any_of(here.begin(), here.end(), [&](const auto& entry) {
               if (!entry.second) return false;
               const auto inside = Entries(root / folder / entry.first);
               return std::any_of(inside.begin(), inside.end(),
                                  [](const auto& file) { return !file.second && EndsWith(Lower(file.first), ".pk3"); });
             })) {
    probe.engine = "idtech3";  // Quake III Arena's engine and its heirs: data in .pk3 files one folder down
  } else {
    probe.engine = marked_engine;
  }
  for (const auto* entries : {&here, &top})
    if (Find(*entries, "easyanticheat", true) || Find(*entries, "battleye", true)) probe.anticheat = true;
  return probe;
}

GameDefaults DefaultsFor(const GameProbe& probe, const RuntimeAbilities& runtime) {
  GameDefaults out;
  const auto has = [&](const char* api) { return probe.apis.count(api) != 0; };
  const bool direct3d = has("d3d8") || has("d3d9") || has("d3d10") || has("d3d11") || has("dxgi");
  // Soft refusals: the detection is broad (a .NET launcher in front of a native game), so the
  // engine defaults below are still worked out for a player who tries anyway.
  std::string soft;
  if (probe.shared_section && !runtime.shared_sections)
    soft = "This game's program needs a shared memory section the PS5 can't map yet.";
  else if (probe.dotnet && !runtime.wine_mono)
    soft = "This game needs .NET, which Playden's runtime doesn't include yet.";
  if (probe.steam_stub && !probe.steam_stub_supported) {
    out.refusal = "This game's program has Steam DRM in a form Playden can't unpack yet.";
  } else if (probe.engine == "nwjs") {
    // Chromium starts helper processes, and in one process it reserves 64 GB of address space.
    out.refusal = "This game runs on NW.js (a built-in web browser), which Playden can't start on the PS5 yet.";
  } else if (probe.engine == "unity") {
    // Unity also names d3d12 and Vulkan; D3D11 is the path DXVK covers. Unity before 2017.2
    // (the player built into the program) still has a Direct3D 9 renderer, which draws on the
    // console (Hexcells); on D3D11 one (140) stopped before its first frame.
    out.graphics = "dxvk";
    out.arguments = probe.legacy_unity ? "-force-d3d9 {resolution}" : "-force-d3d11 {resolution}";
    out.resolution_arguments = "-screen-width {w} -screen-height {h}";
  } else if (probe.engine == "unreal") {
    out.graphics = "dxvk";
    out.arguments = "-dx11";
  } else if (probe.engine == "source") {
    out.graphics = "dxvk";
    out.arguments = "{resolution}";
    out.resolution_arguments = "-w {w} -h {h}";
  } else if (probe.engine == "love" || probe.engine == "sil") {
    // SIL (FTL) draws with OpenGL though its program also names Direct3D 11; under DXVK it
    // quits right after setting up video.
    out.graphics = "opengl";
  } else if (probe.engine == "idtech3") {
    // id Tech 3 copies the OpenGL extension list into a fixed buffer; Mesa's is far longer than any
    // of 2001 and overflows it (Return to Castle Wolfenstein faults reading "rev " as an address).
    out.graphics = "opengl";
    out.environment = "MESA_EXTENSION_MAX_YEAR=2001";
  } else if (probe.engine == "godot") {
    out.graphics = "opengl";
    out.arguments = "--rendering-driver opengl3";
  } else if (direct3d || probe.engine == "gamemaker") {
    out.graphics = "dxvk";
  } else if (has("opengl")) {
    out.graphics = "opengl";
  } else if (has("d3d12")) {
    out.refusal = "This game needs Direct3D 12, which isn't supported on the PS5 yet.";
  }
  if (out.refusal.empty() && !soft.empty()) {
    out.refusal = std::move(soft);
    out.refusal_soft = true;
  }
  std::string facts = probe.engine;
  if (probe.il2cpp) facts += facts.empty() ? "il2cpp" : "-il2cpp";
  if (probe.legacy_unity) facts += "-legacy";
  for (const auto& api : probe.apis) facts += (facts.empty() ? "" : ",") + api;
  if (probe.anticheat) facts += (facts.empty() ? "" : ",") + std::string("anticheat");
  if (probe.shared_section) facts += (facts.empty() ? "" : ",") + std::string("shared-section");
  out.summary = (facts.empty() ? "unknown" : facts) + " graphics=" + out.graphics;
  return out;
}

std::string DetectedLabel(const std::string& summary) {
  const auto space = summary.find(" graphics=");
  if (space == std::string::npos) return {};
  const auto graphics = summary.substr(space + 10);
  std::set<std::string> facts;
  for (std::size_t start = 0; start < space;) {
    const auto end = std::min(summary.find(',', start), space);
    facts.insert(summary.substr(start, end - start));
    start = end + 1;
  }
  std::string engine;
  for (const auto& [id, name] : std::initializer_list<std::pair<const char*, const char*>>{
           {"unity", "Unity"}, {"unity-il2cpp", "Unity"}, {"unity-legacy", "Unity"}, {"unreal", "Unreal Engine"}, {"source", "Source"},
           {"gamemaker", "GameMaker"}, {"godot", "Godot"}, {"love", "L\xC3\x96VE"}, {"nwjs", "NW.js"}, {"sil", "SIL"},
           {"idtech3", "id Tech 3"}})
    if (facts.count(id)) engine = name;
  std::string api;
  if (graphics == "opengl") api = "OpenGL";
  else if (graphics == "dxvk")
    api = facts.count("unity-legacy") ? "Direct3D 9"
              : facts.count("d3d11") || facts.count("dxgi") || engine == "Unity" || engine == "Unreal Engine" ||
                  engine == "GameMaker"
              ? "Direct3D 11"
              : facts.count("d3d10") ? "Direct3D 10" : facts.count("d3d9") ? "Direct3D 9"
              : facts.count("d3d8") ? "Direct3D 8" : "Direct3D";
  if (engine.empty()) return api;
  return api.empty() ? engine : engine + " · " + api;
}

}  // namespace playden::runtime
