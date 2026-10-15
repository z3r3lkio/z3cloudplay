#pragma once
#include "runtime/settings_publication_journal.h"
#include "runtime/artifact_hash.h"
#include "runtime/publication_contract.h"
#include <cstring>
using namespace z3cloudplay::runtime;
template<std::size_t N> inline void Copy(char (&target)[N], const std::string& value) {
  CHECK(value.size() < N); std::memcpy(target, value.c_str(), value.size() + 1);
}
inline void Refresh(SettingsPublicationOperation& o) {
  const auto& r = o.install; const auto& p = o.plan;
  o.descriptor_bytes = "{\"schema_version\":2,\"app_id\":220,\"settings_revision\":" + std::to_string(p.settings_revision) +
      ",\"title_id\":\"FAKE78001\",\"install_id\":\"install-220\",\"data_root\":\"/data/z3cloudplay/games/install-220\","
      "\"default_profile_id\":\"z3cloudplay-steam-220\",\"runtime_bundle_id\":\"wine-wow64\","
      "\"recipe_id\":\"steam-220-hl2-v1\",\"recipe_version\":\"1\",\"exit_policy\":\"exit-title\","
      "\"game_root\":\"" + r.install_path + "\"}\n";
  o.catalog_bytes = p.profile_id + ".profile\n";
  PwTitlePublicationMetadata m{}; m.schema_version = 2;
  Copy(m.install_id, r.install_id); Copy(m.title_id, r.title_id); Copy(m.data_root, r.data_root);
  Copy(m.profile_id, p.profile_id); Copy(m.runtime_bundle_id, r.runtime_bundle_id);
  Copy(m.recipe_id, p.recipe_id); Copy(m.recipe_version, p.recipe_version); Copy(m.kit_id, o.kit_id);
  m.settings_revision = p.settings_revision;
  Copy(m.profile_sha256, *HashArtifactBytes(p.profile_bytes));
  Copy(m.descriptor_sha256, *HashArtifactBytes(o.descriptor_bytes));
  Copy(m.catalog_sha256, *HashArtifactBytes(o.catalog_bytes));
  m.input_count = p.input_presets.size();
  for (std::size_t i = 0; i < p.input_presets.size(); ++i) {
    Copy(m.inputs[i].name, p.input_presets[i].first); Copy(m.inputs[i].sha256, *HashArtifactBytes(p.input_presets[i].second));
  }
  Copy(m.dxvk_path, r.install_path + "/dxvk.conf"); m.dxvk_present = !p.dxvk_config.empty();
  if (m.dxvk_present) Copy(m.dxvk_sha256, *HashArtifactBytes(p.dxvk_config));
  m.registry_count = 2;
  Copy(m.registry[0].key, "Software\\Wine\\AppDefaults\\probe.exe"); Copy(m.registry[0].name, "Version");
  Copy(m.registry[1].key, "Control Panel\\Desktop"); Copy(m.registry[1].name, "LogPixels");
  m.registry[0].kind = m.registry[1].kind = PW_TITLE_PUBLICATION_REGISTRY_ABSENT;
  for (const auto& desired : p.registry) {
    if (desired.name == "Version" && !desired.data.empty()) {
      m.registry[0].kind = PW_TITLE_PUBLICATION_REGISTRY_STRING; Copy(m.registry[0].string_value, "win7");
    }
    if (desired.name == "LogPixels" && !desired.data.empty()) {
      m.registry[1].kind = PW_TITLE_PUBLICATION_REGISTRY_DWORD; m.registry[1].dword = 120;
    }
  }
  auto metadata = SerializePublicationMetadata(m); CHECK(metadata); o.metadata_bytes = *metadata;
}
inline SettingsPublicationOperation Fixture() {
  SettingsPublicationOperation o; o.transaction_id = "00000000-0000-7000-8000-000000000001"; o.kit_id = "kit-hl2";
  auto& r = o.install;
  r.install_id = "install-220"; r.title_id = "FAKE78001"; r.app_id = 220;
  r.data_root = "/data/z3cloudplay/games/install-220"; r.default_profile_id = "z3cloudplay-steam-220";
  r.runtime_bundle_id = "wine-wow64"; r.recipe_id = "steam-220-hl2-v1"; r.recipe_version = "1";
  r.settings_revision = 7; r.launch_entry_id = "main"; r.launch_path = "probe.exe";
  r.install_path = "/data/homebrew/FAKE78001/games/220"; r.volume_id = "internal";
  r.artifacts = {{"wine", "e16ec8f", std::string(64, 'a')}};
  r.last_failure = RuntimeFailure{"prior-failure", "prior message"};
  r.journal = PreparationJournal{"owned-operation", "prepare", "profile"};
  auto& p = o.plan;
  p.app_id = r.app_id; p.recipe_id = r.recipe_id; p.recipe_version = r.recipe_version;
  p.profile_id = r.default_profile_id; p.settings_revision = r.settings_revision;
  p.launch_entry_id = "main"; p.launch_entry = {"main", "Play", "probe.exe", ""};
  p.profile_bytes = "[application]\nid = z3cloudplay-steam-220\nname = Probe\n"
      "executable = Z:\\data\\homebrew\\FAKE78001\\games\\220\\probe.exe\n"
      "working_directory = Z:\\data\\homebrew\\FAKE78001\\games\\220\n"
      "arguments = -steam\nprefix = game\nruntime = wine-wow64\narchitecture = pe64\ngraphics = dxvk\n"
      "[input]\npreset = gamepad\nmode = keyboard\n";
  p.input_presets = {{"gamepad.input", "[input]\nmode = keyboard\ncross = space\n"}};
  Refresh(o); return o;
}
inline void BindPrevious(SettingsPublicationOperation& operation) {
  auto previous = Fixture(); previous.transaction_id = "00000000-0000-7000-8000-000000000002";
  previous.install.settings_revision = previous.plan.settings_revision = operation.previous_revision;
  Refresh(previous); std::string reason;
  auto frozen = FreezeSettingsPublicationOperation(previous, &reason); CHECK(frozen);
  operation.previous_receipt_bytes = frozen->committed_receipt_bytes;
  operation.previous_metadata_bytes = frozen->metadata_bytes;
}
