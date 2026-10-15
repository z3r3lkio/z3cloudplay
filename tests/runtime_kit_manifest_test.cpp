#include "runtime/kit_manifest.h"
#if Z3CLOUDPLAY_TEST_KIT_ADMISSION
#include "runtime/artifact_hash.h"
#include "runtime/kit_admission.h"
#endif

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace z3cloudplay::runtime;

namespace {

std::string HashBytes(std::string_view bytes) {
#if Z3CLOUDPLAY_TEST_KIT_ADMISSION
  return *HashArtifactBytes(bytes);
#else
  std::uint64_t value = 1469598103934665603ULL;
  for (unsigned char byte : bytes) value = (value ^ byte) * 1099511628211ULL;
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned index = 0; index < 4; ++index)
    output << std::setw(16) << (value + index * 0x9e3779b97f4a7c15ULL);
  return output.str();
#endif
}

std::optional<std::string> HashFile(const fs::path& path) {
#if Z3CLOUDPLAY_TEST_KIT_ADMISSION
  return HashArtifactFile(path);
#else
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  return HashBytes(std::string(std::istreambuf_iterator<char>(input), {}));
#endif
}

void Write(const fs::path& path, const std::string& bytes, unsigned mode = 0644) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
  ::chmod(path.c_str(), mode);
}

std::string Pe(bool pe64) {
  std::string bytes(512, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; bytes[0x3c] = 0x80;
  bytes[0x80] = 'P'; bytes[0x81] = 'E';
  const unsigned machine = pe64 ? 0x8664 : 0x014c;
  bytes[0x84] = static_cast<char>(machine & 0xff);
  bytes[0x85] = static_cast<char>(machine >> 8);
  const unsigned magic = pe64 ? 0x020b : 0x010b;
  bytes[0x98] = static_cast<char>(magic & 0xff);
  bytes[0x99] = static_cast<char>(magic >> 8);
  return bytes;
}

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

struct Fixture {
  fs::path root = fs::canonical(fs::temp_directory_path()) /
                  ("z3cloudplay-kit-manifest-" + std::to_string(::getpid()));
  const std::string wrapper = std::string(40, 'a');
  const std::string wine = std::string(40, 'b');
  const std::string helper_revision = std::string(40, 'c');
  const std::string helper_base = std::string(40, 'd');
  const std::string foundation = std::string(40, 'e');
  const std::string dxvk_archive = std::string(64, 'f');
  KitAdmissionConfig trust;
  bool publication_guard = false;
  bool any_title_capability = false;
  std::string variant_fields;  // extra members of the first variant, e.g. ,"any_title":true
  std::string variant_title = "FAKE78001";
  std::string additional_variants;

  // Turns the variant into one shell for every game: template title FAKE78000, a FAKE78 rule
  // helper reading Z3CloudPlay's allow-list, and the capability.
  void MakeAnyTitle(const std::string& allow_list = "/data/z3cloudplay/title-rule/allow-list",
                    const std::string& prefix = "FAKE78") {
    const auto helper = *HashFile(root / "titles/variant/lapy.elf");
    const auto protocol = *HashFile(root / "titles/variant/lapy_elevation_protocol.h");
    const std::string rule = "{\"prefix\":" + Quote(prefix) + ",\"allow_list\":" + Quote(allow_list) + "}";
    Write(root / "titles/variant/sce_sys/param.json", "{\"titleId\":\"FAKE78000\"}");
    Write(root / "titles/variant/lapy-manifest.json",
          "{\"schema\":\"lapy-owned-build/1\",\"mode\":\"elf-helper\",\"target_title\":" + Quote(prefix + "*") +
          ",\"title_rule\":" + rule + ",\"elf_sha256\":" + Quote(helper) + ",\"protocol_sha256\":" + Quote(protocol) +
          ",\"features\":[\"root_layout_probe_retry\",\"title_rule\"]}");
    Write(root / "titles/variant/native-title-build.json",
          "{\"schema\":\"prospero-win-native-title/1\",\"title_id\":\"FAKE78000\",\"any_title\":true,"
          "\"title_rule\":" + rule + ",\"source_revision\":" + Quote(wrapper) + ",\"source_dirty\":false,\"eboot_sha256\":" +
          Quote(*HashFile(root / "titles/variant/eboot.bin")) + "}");
    variant_title = "FAKE78000";
    variant_fields = ",\"any_title\":true";
    any_title_capability = true;
    trust.known_capabilities.push_back("title-any-id");
    RewriteManifest();
  }

  void SetPublicationMarkers(const std::string& fields,
                             const std::string& variant = "variant") {
    const auto path = root / "titles" / variant / "native-title-build.json";
    std::ifstream input(path);
    std::string text(std::istreambuf_iterator<char>(input), {});
    assert(!text.empty() && text.back() == '}');
    text.insert(text.size() - 1, fields);
    Write(path, text);
  }

  void AddSecondVariant() {
    fs::copy(root / "titles/variant", root / "titles/second", fs::copy_options::recursive);
    for (const char* file : {"lapy-manifest.json", "native-title-build.json", "sce_sys/param.json"}) {
      const auto path = root / "titles/second" / file;
      std::ifstream input(path);
      std::string text(std::istreambuf_iterator<char>(input), {});
      const auto at = text.find("FAKE78001");
      assert(at != std::string::npos);
      text.replace(at, 9, "FAKE78002");
      Write(path, text);
    }
    additional_variants =
        ",{\"id\":\"second\",\"title_id\":\"FAKE78002\",\"path\":\"titles/second\","
        "\"wrapper_revision\":" + Quote(wrapper) + ",\"helper_revision\":" + Quote(helper_revision) +
        ",\"helper_manifest\":\"titles/second/lapy-manifest.json\","
        "\"helper_protocol\":\"titles/second/lapy_elevation_protocol.h\","
        "\"native_build_manifest\":\"titles/second/native-title-build.json\"}";
  }

  Fixture() {
    fs::remove_all(root);
    const std::string runtime = "runtime/wine-wow64/artifact-e16";
    const char* core[] = {"ntdll.dll", "kernel32.dll", "kernelbase.dll",
                          "mmdevapi.dll", "dinput8.dll", "rsaenh.dll"};
    std::ostringstream inventory;
    inventory << '{';
    bool first = true;
    auto runtime_file = [&](const std::string& relative, const std::string& bytes,
                            unsigned mode = 0644) {
      Write(root / runtime / relative, bytes, mode);
      if (!first) inventory << ',';
      first = false;
      inventory << Quote(relative) << ':' << Quote(*HashFile(root / runtime / relative));
    };
    for (const char* name : core) {
      runtime_file("lib/wine/i386-windows/" + std::string(name), Pe(false));
      runtime_file("lib/wine/x86_64-windows/" + std::string(name), Pe(true));
    }
    runtime_file("lib/wine/x86_64-windows/wowprospero.dll", Pe(true));
    for (const char* name : {"ntdll.prx", "wineserver.prx", "win32u.prx",
                             "wineps5.prx", "libvulkan.prx", "wowprospero.prx"})
      runtime_file("lib/wine/x86_64-unix/" + std::string(name), "prx-" + std::string(name), 0755);
    inventory << '}';
    Write(root / runtime / "runtime-inventory.json", inventory.str());

    const fs::path bottle = root / "template/bottle";
    for (const char* name : core) {
      Write(bottle / "drive_c/windows/system32" / name, Pe(true));
      Write(bottle / "drive_c/windows/syswow64" / name, Pe(false));
    }
    Write(bottle / "drive_c/windows/system32/wowprospero.dll", Pe(true));
    const char* dxvk[] = {"d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"};
    for (const char* name : dxvk) {
      Write(bottle / "drive_c/windows/system32" / name, Pe(true));
      Write(bottle / "drive_c/windows/syswow64" / name, Pe(false));
    }
    fs::create_directories(bottle / "drive_c/users/prospero/Desktop");
    fs::create_directories(bottle / "drive_c/users/prospero/Documents");
    Write(bottle / "dosdevices/.pw-symlinks", "c:\t../drive_c\nz:\t/\n");
    WriteRegistry(true);
    Write(root / "recipes/steam-220-hl2-v1.json",
          "{\"id\":\"steam-220-hl2-v1\",\"version\":\"1\",\"requirements\":{\"audio\":\"ps5\"}}");
    Write(root / "titles/variant/eboot.bin", "eboot", 0755);
    Write(root / "titles/variant/lapy.elf", "helper", 0755);
    Write(root / "titles/variant/lapy_elevation_protocol.h", "protocol");
    Write(root / "titles/variant/sce_sys/param.json", "{\"titleId\":\"FAKE78001\"}");
    RefreshInnerManifests();

    trust.required_architecture = "x86_64+wow64-i386";
    trust.known_capabilities = {"wine-wow64", "ps5-audio"};
    trust.hash_file = HashFile;
    trust.hash_bytes = [](std::string_view bytes) -> std::optional<std::string> {
      return HashBytes(bytes);
    };
    RewriteManifest();
  }

  ~Fixture() { fs::remove_all(root); }

  void WriteRegistry(bool valid) {
    const std::string crypto64 =
        "[Software\\\\Microsoft\\\\Cryptography\\\\Defaults\\\\Provider Types\\\\Type 001]\n\"Name\"=\"Microsoft Enhanced Cryptographic Provider v1.0\"\n"
        "[Software\\\\Microsoft\\\\Cryptography\\\\Defaults\\\\Provider\\\\Microsoft Enhanced Cryptographic Provider v1.0]\n\"Image Path\"=\"C:\\\\windows\\\\system32\\\\rsaenh.dll\"\n\"Type\"=dword:00000001\n";
    const std::string crypto32 =
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Cryptography\\\\Defaults\\\\Provider Types\\\\Type 001]\n\"Name\"=\"Microsoft Enhanced Cryptographic Provider v1.0\"\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Cryptography\\\\Defaults\\\\Provider\\\\Microsoft Enhanced Cryptographic Provider v1.0]\n\"Image Path\"=\"C:\\\\windows\\\\system32\\\\rsaenh.dll\"\n\"Type\"=dword:00000001\n";
    const std::string com =
        "[Software\\\\Classes\\\\Wow6432Node\\\\CLSID\\\\{BCDE0395-E52F-467C-8E3D-C4579291692E}]\n"
        "[Software\\\\Classes\\\\Wow6432Node\\\\CLSID\\\\{BCDE0395-E52F-467C-8E3D-C4579291692E}\\\\InprocServer32]\n@=\"C:\\\\windows\\\\system32\\\\mmdevapi.dll\"\n\"ThreadingModel\"=\"Both\"\n"
        "[Software\\\\Classes\\\\Wow6432Node\\\\CLSID\\\\{25E609E4-B259-11CF-BFC7-444553540000}]\n"
        "[Software\\\\Classes\\\\Wow6432Node\\\\CLSID\\\\{25E609E4-B259-11CF-BFC7-444553540000}\\\\InprocServer32]\n@=\"C:\\\\windows\\\\system32\\\\dinput8.dll\"\n\"ThreadingModel\"=\"Both\"\n";
    Write(root / "template/bottle/system.reg",
          std::string("[Software\\\\Microsoft\\\\Wow64\\\\x86]\n@=\"") +
              (valid ? "wowprospero.dll" : "wrong.dll") + "\"\n" + crypto64 + crypto32 + com);
    Write(root / "template/bottle/user.reg",
          "[Software\\\\Wine\\\\Drivers]\n\"Audio\"=\"ps5\"\n"
          "[Volatile Environment]\n\"USERNAME\"=\"prospero\"\n\"USERPROFILE\"=\"C:\\\\users\\\\prospero\"\n");
  }

  void RefreshInnerManifests(bool dirty = false) {
    const auto helper = *HashFile(root / "titles/variant/lapy.elf");
    const auto protocol = *HashFile(root / "titles/variant/lapy_elevation_protocol.h");
    Write(root / "titles/variant/lapy-manifest.json",
          "{\"schema\":\"lapy-owned-build/1\",\"mode\":\"elf-helper\",\"target_title\":\"FAKE78001\","
          "\"elf_sha256\":" + Quote(helper) + ",\"protocol_sha256\":" + Quote(protocol) +
          ",\"features\":[\"root_layout_probe_retry\"]}");
    Write(root / "titles/variant/native-title-build.json",
          "{\"schema\":\"prospero-win-native-title/1\",\"title_id\":\"FAKE78001\","
          "\"source_revision\":" + Quote(wrapper) + ",\"source_dirty\":" + (dirty ? "true" : "false") + ",\"eboot_sha256\":" +
          Quote(*HashFile(root / "titles/variant/eboot.bin")) + "}");
  }

  void RewriteRuntimeInventory() {
    const fs::path runtime = root / "runtime/wine-wow64/artifact-e16";
    std::ostringstream out; out << '{'; bool first = true;
    for (const auto& item : fs::recursive_directory_iterator(runtime)) {
      if (!item.is_regular_file() || item.path().filename() == "runtime-inventory.json") continue;
      if (!first) out << ','; first = false;
      out << Quote(fs::relative(item.path(), runtime).generic_string()) << ':' << Quote(*HashFile(item.path()));
    }
    out << '}'; Write(runtime / "runtime-inventory.json", out.str());
  }

  void RewriteManifest(std::string firmware = "12.02", std::string runtime_path =
                       "runtime/wine-wow64/artifact-e16") {
    std::vector<KitFile> entries;
    for (const auto& item : fs::recursive_directory_iterator(root)) {
      if (!item.is_regular_file() || item.path().filename() == "manifest.json") continue;
      const auto status = fs::status(item.path());
      entries.push_back({fs::relative(item.path(), root).generic_string(), item.file_size(),
                         *HashFile(item.path()), static_cast<unsigned>(status.permissions()) & 0777});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    std::ostringstream lines, files;
    files << '[';
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto& e = entries[i];
      std::ostringstream mode; mode << std::oct << std::setw(4) << std::setfill('0') << e.mode;
      lines << e.path << '\t' << e.size << '\t' << e.sha256 << '\t' << mode.str() << '\n';
      if (i) files << ',';
      files << "{\"path\":" << Quote(e.path) << ",\"size\":" << e.size
            << ",\"sha256\":" << Quote(e.sha256) << ",\"mode\":" << Quote(mode.str()) << '}';
    }
    files << ']';
    auto source = [&](const char* name, const std::string& body) {
      return Quote(name) + ":{" + body + ",\"url\":\"https://example.invalid/source\"}";
    };
    std::ostringstream dxvk;
    dxvk << "{\"version\":\"2.6.2\",\"files\":{";
    for (const auto& pair : {std::pair{"x64", "system32"}, std::pair{"x32", "syswow64"}}) {
      if (pair.first == std::string("x32")) dxvk << ',';
      dxvk << Quote(pair.first) << ":{";
      bool first = true;
      for (const char* name : {"d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"}) {
        if (!first) dxvk << ','; first = false;
        dxvk << Quote(name) << ':' << Quote(*HashFile(root / "template/bottle/drive_c/windows" / pair.second / name));
      }
      dxvk << '}';
    }
    dxvk << "}}";
    const std::string sources = "{" +
      source("wrapper", "\"revision\":" + Quote(wrapper) + ",\"baseline_revision\":" + Quote(wrapper)) + "," +
      source("wine", "\"revision\":" + Quote(wine)) + "," +
      source("helper", "\"revision\":" + Quote(helper_revision) + ",\"base_revision\":" + Quote(helper_base)) + "," +
      source("foundation", "\"revision\":" + Quote(foundation)) + "," +
      source("sdk", "\"version\":\"0.42\"") + "," +
      source("dxvk", "\"version\":\"2.6.2\",\"archive_sha256\":" + Quote(dxvk_archive)) + "}";
    const std::string manifest =
      "{\"schema\":\"z3cloudplay-prospero-kit/1\",\"kit_id\":\"kit-1\",\"firmware\":[" + Quote(firmware) +
      "],\"architecture\":\"x86_64+wow64-i386\",\"capabilities\":[" +
      (publication_guard ? "\"z3cloudplay-publication-guard-v1\"," : "") + "\"ps5-audio\"," +
      (any_title_capability ? "\"title-any-id\"," : "") + "\"wine-wow64\"],"
      "\"sources\":" + sources + ",\"runtime\":{\"bundle_id\":\"wine-wow64\",\"artifact_revision\":\"artifact-e16\","
      "\"wine_revision\":" + Quote(wine) + ",\"path\":" + Quote(runtime_path) +
      ",\"inventory_manifest\":\"runtime/wine-wow64/artifact-e16/runtime-inventory.json\","
      "\"module_counts\":{\"i386-windows\":6,\"x86_64-windows\":7,\"x86_64-unix\":6}},"
      "\"template\":{\"path\":\"template/bottle\",\"user\":\"prospero\"},"
      "\"recipe\":{\"id\":\"steam-220-hl2-v1\",\"version\":\"1\",\"path\":\"recipes/steam-220-hl2-v1.json\",\"sha256\":" +
      Quote(*HashFile(root / "recipes/steam-220-hl2-v1.json")) + "},\"variants\":[{\"id\":\"variant\",\"title_id\":" + Quote(variant_title) + variant_fields + ","
      "\"path\":\"titles/variant\",\"wrapper_revision\":" + Quote(wrapper) + ",\"helper_revision\":" + Quote(helper_revision) +
      ",\"helper_manifest\":\"titles/variant/lapy-manifest.json\",\"helper_protocol\":\"titles/variant/lapy_elevation_protocol.h\","
      "\"native_build_manifest\":\"titles/variant/native-title-build.json\"}" + additional_variants + "],\"dxvk\":" + dxvk.str() +
      ",\"files\":" + files.str() + ",\"files_sha256\":" + Quote(HashBytes(lines.str())) + "}";
    Write(root / "manifest.json", manifest);
    trust.expected_manifest_sha256 = std::string(64, '0');
  }

  KitManifestResult Verify() const {
#if Z3CLOUDPLAY_TEST_KIT_ADMISSION
    return VerifyInstalledKit(root, trust);
#else
    return VerifyKitManifest(root, trust);
#endif
  }
};

void CheckRejected(const KitManifestResult& result, KitManifestStatus status) {
  assert(!result.view);
  assert(result.status == status);
  assert(!result.reason.empty());
}

}  // namespace

int main() {
  const std::string guard = "z3cloudplay-publication-guard-v1";
  // Independent reviewed source-provider provenance digest, not read from the kit.
  const std::string provider = "f14d5e3ecb31951e3cf5bfdc41972181a9be1d96ed0dc7cb3aeebb8e4aab844a";
  const std::string valid_guard_fields =
      ",\"managed_publication_guard\":" + Quote(guard) +
      ",\"managed_descriptor_required\":true,\"publication_provider_sha256\":" + Quote(provider);
  {
    Fixture fixture;
    fixture.trust.known_capabilities.push_back(guard);
    // Incomplete opt-in trust configuration must fail even for a legacy kit.
    CheckRejected(fixture.Verify(), KitManifestStatus::kUnsupportedKit);
    fixture.trust.publication_provider_sha256 = std::string(64, 'A');
    CheckRejected(fixture.Verify(), KitManifestStatus::kUnsupportedKit);
    fixture.trust.publication_provider_sha256 = provider;
    assert(fixture.Verify().status == KitManifestStatus::kVerified);
  }
  {
    Fixture fixture;
    fixture.publication_guard = true;
    fixture.SetPublicationMarkers(valid_guard_fields);
    fixture.RewriteManifest();
    fixture.trust.publication_provider_sha256 = provider;
    // Supplying a digest does not make the capability known: the kit is admitted without it.
    const auto result = fixture.Verify();
    assert(result.status == KitManifestStatus::kVerified);
    assert(result.ignored_capabilities == std::vector<std::string>{guard});
    assert(std::find(result.capabilities.begin(), result.capabilities.end(), guard) == result.capabilities.end());
  }
  {
    // One shell for every game is admitted with its capability and Z3CloudPlay's title rule.
    Fixture fixture;
    fixture.MakeAnyTitle();
    const auto result = fixture.Verify();
    assert(result.status == KitManifestStatus::kVerified && result.view);
    assert(result.view->variants.size() == 1 && result.view->variants[0].any_title &&
           result.view->variants[0].title_id == "FAKE78000");
    assert(KitVariantFor(*result.view, "FAKE78042") == &result.view->variants[0]);
  }
  {
    // A build that doesn't know the capability ignores it and so refuses the variant.
    Fixture fixture;
    fixture.MakeAnyTitle();
    fixture.trust.known_capabilities.pop_back();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    // Without the capability, or without Z3CloudPlay's allow-list or range, it is refused.
    Fixture without;
    without.MakeAnyTitle();
    without.any_title_capability = false;
    without.RewriteManifest();
    CheckRejected(without.Verify(), KitManifestStatus::kIntegrityFailure);
    Fixture other_list;
    other_list.MakeAnyTitle("/data/elsewhere/allow-list");
    CheckRejected(other_list.Verify(), KitManifestStatus::kIntegrityFailure);
    Fixture other_range;
    other_range.MakeAnyTitle("/data/z3cloudplay/title-rule/allow-list", "FAKE7");
    CheckRejected(other_range.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    // The capability with only fixed titles, or an any-title shell listed as a fixed title.
    Fixture capability_only;
    capability_only.any_title_capability = true;
    capability_only.trust.known_capabilities.push_back("title-any-id");
    capability_only.RewriteManifest();
    CheckRejected(capability_only.Verify(), KitManifestStatus::kIntegrityFailure);
    Fixture unmarked;
    unmarked.MakeAnyTitle();
    unmarked.variant_fields.clear();
    unmarked.RewriteManifest();
    CheckRejected(unmarked.Verify(), KitManifestStatus::kIntegrityFailure);
    Fixture malformed;
    malformed.MakeAnyTitle();
    malformed.variant_fields = ",\"any_title\":\"yes\"";
    malformed.RewriteManifest();
    CheckRejected(malformed.Verify(), KitManifestStatus::kMalformed);
  }
  const std::vector<std::string> invalid_guard_fields = {
      "",
      ",\"managed_publication_guard\":true,\"managed_descriptor_required\":true,\"publication_provider_sha256\":" + Quote(provider),
      ",\"managed_publication_guard\":\"other\",\"managed_descriptor_required\":true,\"publication_provider_sha256\":" + Quote(provider),
      ",\"managed_publication_guard\":" + Quote(guard) + ",\"publication_provider_sha256\":" + Quote(provider),
      ",\"managed_publication_guard\":" + Quote(guard) + ",\"managed_descriptor_required\":false,\"publication_provider_sha256\":" + Quote(provider),
      ",\"managed_publication_guard\":" + Quote(guard) + ",\"managed_descriptor_required\":\"true\",\"publication_provider_sha256\":" + Quote(provider),
      ",\"managed_publication_guard\":" + Quote(guard) + ",\"managed_descriptor_required\":true",
      ",\"managed_publication_guard\":" + Quote(guard) + ",\"managed_descriptor_required\":true,\"publication_provider_sha256\":true",
      ",\"managed_publication_guard\":" + Quote(guard) + ",\"managed_descriptor_required\":true,\"publication_provider_sha256\":" + Quote(std::string(64, 'a')),
  };
  for (const auto& fields : invalid_guard_fields) {
    Fixture fixture;
    fixture.publication_guard = true;
    fixture.trust.known_capabilities.push_back(guard);
    fixture.trust.publication_provider_sha256 = provider;
    fixture.SetPublicationMarkers(fields);
    fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture;
    fixture.publication_guard = true;
    fixture.trust.known_capabilities.push_back(guard);
    fixture.trust.publication_provider_sha256 = provider;
    fixture.AddSecondVariant(); // First and second are legacy native-build manifests.
    fixture.SetPublicationMarkers(valid_guard_fields);
    fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
    fixture.SetPublicationMarkers(valid_guard_fields, "second");
    fixture.RewriteManifest();
    const auto result = fixture.Verify();
    assert(result.status == KitManifestStatus::kVerified);
    assert(result.available_variant_ids == std::vector<std::string>({"variant", "second"}));
  }
  {
    Fixture fixture;
    const auto result = fixture.Verify();
    if (result.status != KitManifestStatus::kVerified)
      std::fprintf(stderr, "valid fixture rejected: %s\n", result.reason.c_str());
    assert(result.status == KitManifestStatus::kVerified);
    assert(result.view && result.view->kit_id == "kit-1");
    assert(result.available_variant_ids == std::vector<std::string>{"variant"});
    assert(result.capabilities == std::vector<std::string>({"ps5-audio", "wine-wow64"}));
    assert(result.artifacts.size() >= 2);
    // What the kit was built from is reported, whatever it is.
    assert(result.manifest_sha256 == *HashFile(fixture.root / "manifest.json"));
    assert(result.sources.at("wine").at("revision") == fixture.wine);
    assert(result.sources.at("wrapper").at("revision") == fixture.wrapper);
    assert(result.ignored_capabilities.empty() && !result.source_dirty);
  }
  {
    // A kit built from uncommitted changes, with a capability this build doesn't know.
    Fixture fixture;
    fixture.trust.known_capabilities = {"wine-wow64"};
    fixture.RefreshInnerManifests(true); fixture.RewriteManifest();
    const auto result = fixture.Verify();
    assert(result.status == KitManifestStatus::kVerified && result.source_dirty);
    assert(result.capabilities == std::vector<std::string>{"wine-wow64"});
    assert(result.ignored_capabilities == std::vector<std::string>{"ps5-audio"});
  }
  {
    // TypeName is descriptive: a real Wine registration selects this provider
    // independently for each view. The Base provider is also legitimate.
    Fixture fixture;
    const auto path = fixture.root / "template/bottle/system.reg";
    std::ifstream input(path);
    std::string registry(std::istreambuf_iterator<char>(input), {});
    const std::string enhanced = "Microsoft Enhanced Cryptographic Provider v1.0";
    const std::string base = "Microsoft Base Cryptographic Provider v1.0";
    for (unsigned i = 0; i < 2; ++i) {
      const auto pos = registry.find(enhanced);
      assert(pos != std::string::npos);
      registry.replace(pos, enhanced.size(), base);
    }
    Write(path, registry); fixture.RewriteManifest();
    assert(fixture.Verify().status == KitManifestStatus::kVerified);
  }
  {
    // An unregistered selected provider must not fall back to an unrelated
    // rsaenh registration that happens to be present elsewhere in the file.
    Fixture fixture;
    const auto path = fixture.root / "template/bottle/system.reg";
    std::ifstream input(path);
    std::string registry(std::istreambuf_iterator<char>(input), {});
    const auto pos = registry.find("Microsoft Enhanced Cryptographic Provider v1.0");
    assert(pos != std::string::npos);
    registry.replace(pos, std::string("Microsoft Enhanced Cryptographic Provider v1.0").size(),
                     "Absent Provider");
    Write(path, registry); fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture;
    const auto path = fixture.root / "template/bottle/system.reg";
    std::ifstream input(path);
    std::string registry(std::istreambuf_iterator<char>(input), {});
    const auto pos = registry.find("\"Name\"=");
    assert(pos != std::string::npos);
    registry.insert(pos, "\"Name\"=\"Ambiguous Provider\"\n");
    Write(path, registry); fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture;
    Write(fixture.root / "template/bottle/user.reg",
          "[Software\\\\Wine\\\\Drivers]\n\"Audio\"=\"ps5\"\n"
          "[Volatile Environment]\n\"USERNAME\"=\"other\"\n\"USERPROFILE\"=\"C:\\\\users\\\\other\"\n"
          "; expected prospero C:\\\\users\\\\prospero\n");
    fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture; Write(fixture.root / "unexpected.bin", "extra");
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture;
    Write(fixture.root / "runtime/wine-wow64/artifact-e16/lib/wine/i386-windows/ntdll.dll", Pe(true));
    fixture.RewriteRuntimeInventory(); fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    // A helper that differs from its own helper manifest is refused; a rebuilt one is used.
    Fixture fixture;
    Write(fixture.root / "titles/variant/lapy.elf", "other helper", 0755);
    fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
    fixture.RefreshInnerManifests(); fixture.RewriteManifest();
    assert(fixture.Verify().status == KitManifestStatus::kVerified);
  }
  {
    // Any console firmware is admitted; the kit's list is only what it was tested on.
    Fixture fixture; fixture.RewriteManifest("99.99");
    assert(fixture.Verify().status == KitManifestStatus::kVerified);
  }
  {
    Fixture fixture; fixture.RewriteManifest("12.02", "../escape");
    CheckRejected(fixture.Verify(), KitManifestStatus::kMalformed);
  }
  {
    Fixture fixture;
    fs::create_symlink("recipes/steam-220-hl2-v1.json", fixture.root / "alias");
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture; fixture.WriteRegistry(false); fixture.RewriteManifest();
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
#if Z3CLOUDPLAY_TEST_KIT_ADMISSION
  {
    Fixture fixture;
    // Admission must bind real SHA-256 rather than trusting caller callbacks.
    fixture.trust.hash_file = [](const fs::path&) -> std::optional<std::string> {
      return std::nullopt;
    };
    fixture.trust.hash_bytes = [](std::string_view) -> std::optional<std::string> {
      return std::string(64, '0');
    };
    assert(fixture.Verify().status == KitManifestStatus::kVerified);
    Write(fixture.root / "titles/variant/eboot.bin", "changed", 0755);
    CheckRejected(fixture.Verify(), KitManifestStatus::kIntegrityFailure);
  }
  {
    Fixture fixture;
    // Kit files are not hashed at admission: the manifest's hashes name the version, so a
    // same-size edit without a new manifest is admitted as the version the manifest names.
    Write(fixture.root / "titles/variant/eboot.bin", "other", 0755);
    assert(fixture.Verify().status == KitManifestStatus::kVerified);
  }
  {
    Fixture fixture;
    const auto ancestor = fixture.root.parent_path() / (fixture.root.filename().string() + "-alias");
    fs::create_directory_symlink(fixture.root.parent_path(), ancestor);
    const auto result = VerifyInstalledKit(ancestor / fixture.root.filename(), fixture.trust);
    fs::remove(ancestor);
    CheckRejected(result, KitManifestStatus::kIntegrityFailure);
  }
#endif
  std::puts("runtime_kit_manifest_test: all assertions passed");
}
