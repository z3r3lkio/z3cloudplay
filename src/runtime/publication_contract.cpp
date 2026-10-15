#include "runtime/publication_contract.h"

#include <charconv>
#include <cstring>
#include <string_view>

namespace playden::runtime {
namespace {
constexpr char kHex[] = "0123456789abcdef";

template <std::size_t N>
std::optional<std::string_view> Bounded(const char (&field)[N]) {
  const auto end = static_cast<const char*>(std::memchr(field, 0, N));
  if (!end) return std::nullopt;
  return std::string_view(field, static_cast<std::size_t>(end - field));
}

std::string Decimal(std::uint64_t number) {
  char text[32];
  const auto result = std::to_chars(text, text + sizeof(text), number);
  return std::string(text, result.ptr);
}

template <std::size_t N>
bool JsonField(std::string& output, const char* key, const char (&field)[N]) {
  const auto value = Bounded(field);
  if (!value) return false;
  output += ",\""; output += key; output += "\":\"";
  for (const unsigned char c : *value) {
    if (c < 32 || c > 126) return false;
    if (c == '"' || c == '\\') output += '\\';
    output += static_cast<char>(c);
  }
  output += '"';
  return true;
}

template <std::size_t N>
bool MarkerField(std::string& output, const std::string& key, const char (&field)[N], bool hex = false) {
  const auto value = Bounded(field);
  if (!value) return false;
  output += key; output += '=';
  for (const unsigned char c : *value) {
    if (c < 32 || c > 126) return false;
    if (hex) { output += kHex[c >> 4]; output += kHex[c & 15]; }
    else output += static_cast<char>(c);
  }
  output += '\n';
  return true;
}

void NumberField(std::string& output, const std::string& key, std::uint64_t number) {
  output += key; output += '='; output += Decimal(number); output += '\n';
}
}

std::optional<std::string> SerializePublicationReceipt(const PwTitlePublication& receipt) {
  const char* phase = receipt.phase == PW_TITLE_PUBLICATION_PENDING ? "pending" :
                      receipt.phase == PW_TITLE_PUBLICATION_COMMITTED ? "committed" : nullptr;
  if (!phase) return std::nullopt;
  std::string output = "{\"schema_version\":" + Decimal(receipt.schema_version) +
                       ",\"phase\":\"" + phase + '"';
  if (!JsonField(output, "transaction_id", receipt.transaction_id) ||
      !JsonField(output, "install_id", receipt.install_id) ||
      !JsonField(output, "title_id", receipt.title_id) ||
      !JsonField(output, "kit_id", receipt.kit_id) ||
      !JsonField(output, "runtime_bundle_id", receipt.runtime_bundle_id) ||
      !JsonField(output, "recipe_id", receipt.recipe_id) ||
      !JsonField(output, "recipe_version", receipt.recipe_version)) return std::nullopt;
  output += ",\"previous_revision\":" + Decimal(receipt.previous_revision);
  output += ",\"settings_revision\":" + Decimal(receipt.settings_revision);
  if (!JsonField(output, "plan_sha256", receipt.plan_sha256) ||
      !JsonField(output, "publication_sha256", receipt.publication_sha256)) return std::nullopt;
  output += "}\n";
  PwTitlePublication parsed{};
  if (pw_title_publication_parse(reinterpret_cast<const uint8_t*>(output.data()), output.size(), &parsed) != PW_OK)
    return std::nullopt;
  return output;
}

std::optional<std::string> SerializePublicationMetadata(const PwTitlePublicationMetadata& metadata) {
  if (metadata.input_count > PW_TITLE_PUBLICATION_MAX_INPUTS ||
      metadata.registry_count > PW_TITLE_PUBLICATION_MAX_REGISTRY) return std::nullopt;
  std::string output;
  NumberField(output, "schema", metadata.schema_version);
  if (!MarkerField(output, "install_id", metadata.install_id) ||
      !MarkerField(output, "title_id", metadata.title_id) ||
      !MarkerField(output, "data_root", metadata.data_root) ||
      !MarkerField(output, "profile_id", metadata.profile_id) ||
      !MarkerField(output, "runtime_bundle_id", metadata.runtime_bundle_id) ||
      !MarkerField(output, "recipe_id", metadata.recipe_id) ||
      !MarkerField(output, "recipe_version", metadata.recipe_version)) return std::nullopt;
  NumberField(output, "settings_revision", metadata.settings_revision);
  if (!MarkerField(output, "profile_sha256", metadata.profile_sha256) ||
      !MarkerField(output, "descriptor_sha256", metadata.descriptor_sha256) ||
      !MarkerField(output, "catalog_sha256", metadata.catalog_sha256) ||
      !MarkerField(output, "kit_id", metadata.kit_id)) return std::nullopt;
  NumberField(output, "input_count", metadata.input_count);
  for (unsigned i = 0; i < metadata.input_count; ++i) {
    const auto prefix = "input." + Decimal(i);
    if (!MarkerField(output, prefix + ".name_hex", metadata.inputs[i].name, true) ||
        !MarkerField(output, prefix + ".sha256", metadata.inputs[i].sha256)) return std::nullopt;
  }
  if (!MarkerField(output, "dxvk.path_hex", metadata.dxvk_path, true)) return std::nullopt;
  NumberField(output, "dxvk.present", metadata.dxvk_present);
  if (!MarkerField(output, "dxvk.sha256", metadata.dxvk_sha256)) return std::nullopt;
  if (metadata.schema_version >= 3) NumberField(output, "game_root.unowned", metadata.game_root_unowned);
  NumberField(output, "registry_count", metadata.registry_count);
  for (unsigned i = 0; i < metadata.registry_count; ++i) {
    const auto& entry = metadata.registry[i];
    const auto prefix = "registry." + Decimal(i);
    if (!MarkerField(output, prefix + ".key_hex", entry.key, true) ||
        !MarkerField(output, prefix + ".name_hex", entry.name, true)) return std::nullopt;
    const char* kind = entry.kind == PW_TITLE_PUBLICATION_REGISTRY_ABSENT ? "absent" :
                       entry.kind == PW_TITLE_PUBLICATION_REGISTRY_STRING ? "string" :
                       entry.kind == PW_TITLE_PUBLICATION_REGISTRY_DWORD ? "dword" : nullptr;
    if (!kind) return std::nullopt;
    const auto value = Bounded(entry.string_value);
    if (!value || (entry.kind != PW_TITLE_PUBLICATION_REGISTRY_STRING && !value->empty()) ||
        (entry.kind != PW_TITLE_PUBLICATION_REGISTRY_DWORD && entry.dword != 0)) return std::nullopt;
    output += prefix + ".kind=" + kind + '\n';
    if (!MarkerField(output, prefix + ".string_hex", entry.string_value, true)) return std::nullopt;
    output += prefix + ".dword=";
    if (entry.kind == PW_TITLE_PUBLICATION_REGISTRY_DWORD)
      for (int shift = 28; shift >= 0; shift -= 4) output += kHex[(entry.dword >> shift) & 15];
    output += '\n';
  }
  PwTitlePublicationMetadata parsed{};
  if (pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(output.data()), output.size(), &parsed) != PW_OK)
    return std::nullopt;
  return output;
}

}  // namespace playden::runtime
