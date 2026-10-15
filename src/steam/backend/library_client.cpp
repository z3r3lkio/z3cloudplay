#include "steam/backend/library_client.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace z3cloudplay::steam {
namespace {

constexpr std::size_t kBodyLimit = 16 * 1024 * 1024;
constexpr std::size_t kGameLimit = 10000;
constexpr std::size_t kTitleLimit = 4096;
constexpr std::size_t kTokenLimit = 16384;
constexpr std::uint32_t kRequestTimeoutMs = 30000;

LibraryResult Failure(LibraryError error) { return {error, {}, false}; }

bool SessionUsable(const StoredSession& session) {
  if (!session.steam_id || session.access_token.empty() ||
      session.access_token.size() > kTokenLimit) {
    return false;
  }
  return std::all_of(session.access_token.begin(), session.access_token.end(),
                     [](unsigned char byte) { return byte >= 0x20 && byte != 0x7f; });
}

bool Unreserved(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
         byte == '_' || byte == '~';
}

std::string PercentEncode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (const unsigned char byte : value) {
    if (Unreserved(byte)) {
      encoded.push_back(static_cast<char>(byte));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[byte >> 4]);
      encoded.push_back(hex[byte & 0x0f]);
    }
  }
  return encoded;
}

bool IconHashValid(std::string_view hash) {
  if (hash.size() != 40) return false;
  return std::all_of(hash.begin(), hash.end(), [](unsigned char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
           (byte >= 'A' && byte <= 'F');
  });
}

bool ParsePlaytime(const json::Value* value,
                   std::optional<std::uint64_t>* minutes) {
  if (!value) {
    minutes->reset();
    return true;
  }
  if (const auto parsed = value->AsUint64()) {
    *minutes = *parsed;
    return true;
  }
  if (!value->is_number() || value->text().empty() || value->text()[0] != '-')
    return false;
  std::int64_t negative = 0;
  const auto parsed = std::from_chars(value->text().data(),
                                      value->text().data() + value->text().size(),
                                      negative);
  if (parsed.ec != std::errc{} || parsed.ptr != value->text().data() + value->text().size() ||
      negative > 0) {
    return false;
  }
  *minutes = 0;
  return true;
}

bool ParseLastPlayed(const json::Value* value,
                     std::optional<std::uint64_t>* seconds) {
  if (!value) {
    seconds->reset();
    return true;
  }
  const auto parsed = value->AsUint64();
  if (!parsed || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  if (*parsed == 0) seconds->reset();
  else *seconds = *parsed;
  return true;
}

LibraryError MapTransport(const HttpResponse& response) {
  if (response.error == HttpError::kCanceled) return LibraryError::kCanceled;
  if (!response.ok()) return LibraryError::kNetwork;
  if (response.status_code == 401 || response.status_code == 403)
    return LibraryError::kCredentialsRevoked;
  if (response.status_code == 429 || response.status_code >= 500)
    return LibraryError::kNetwork;
  if (response.status_code != 200) return LibraryError::kProtocol;
  return LibraryError::kNone;
}

}  // namespace

LibraryClient::LibraryClient(std::string ca_bundle)
    : ca_bundle_(std::move(ca_bundle)) {}

LibraryClient::LibraryClient(LibraryClientHooks hooks)
    : hooks_(std::move(hooks)) {}

LibraryResult LibraryClient::Fetch(const StoredSession& session,
                                   const std::atomic<bool>& cancel) {
  if (cancel.load()) return Failure(LibraryError::kCanceled);
  if (!SessionUsable(session)) return Failure(LibraryError::kNoSession);

  HttpRequest request;
  request.method = HttpMethod::kGet;
  request.url =
      "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"
      "?access_token=" + PercentEncode(session.access_token) +
      "&steamid=" + std::to_string(session.steam_id) +
      "&include_appinfo=true&include_played_free_games=true&format=json";
  request.headers = {{"User-Agent", "Z3CloudPlay/1.0"}};
  request.timeout_ms = kRequestTimeoutMs;
  request.max_response_bytes = kBodyLimit;

  HttpResponse response;
  if (hooks_.perform) {
    response = hooks_.perform(request, cancel);
  } else {
    if (ca_bundle_.empty()) return Failure(LibraryError::kProtocol);
    if (!http_) http_ = std::make_unique<HttpClient>(ca_bundle_);
    response = http_->Perform(request, cancel);
  }
  if (cancel.load()) return Failure(LibraryError::kCanceled);
  const auto transport_error = MapTransport(response);
  if (transport_error != LibraryError::kNone) return Failure(transport_error);
  if (response.body.size() > kBodyLimit) return Failure(LibraryError::kProtocol);

  const char* body_data = response.body.empty()
                              ? ""
                              : reinterpret_cast<const char*>(response.body.data());
  const std::string_view body(body_data, response.body.size());
  json::Limits limits;
  limits.max_bytes = kBodyLimit;
  limits.max_depth = 16;
  limits.max_values = 200000;
  limits.max_string_bytes = 65536;
  const auto root = json::Parse(body, limits);
  if (cancel.load()) return Failure(LibraryError::kCanceled);
  if (!root || !root->is_object()) return Failure(LibraryError::kProtocol);
  const auto* payload = root->Find("response");
  if (!payload || !payload->is_object()) return Failure(LibraryError::kProtocol);

  const auto* count_value = payload->Find("game_count");
  const auto count = count_value ? count_value->AsUint64() : std::nullopt;
  if (count_value && (!count || *count > kGameLimit))
    return Failure(LibraryError::kProtocol);
  const auto* games_value = payload->Find("games");
  if (!games_value) {
    if (!count || *count != 0) return Failure(LibraryError::kProtocol);
    if (cancel.load()) return Failure(LibraryError::kCanceled);
    return {LibraryError::kNone, {}, false};
  }
  if (!games_value->is_array() || games_value->array().size() > kGameLimit ||
      (count && games_value->array().size() != *count)) {
    return Failure(LibraryError::kProtocol);
  }

  LibraryResult result;
  result.error = LibraryError::kNone;
  result.games.reserve(games_value->array().size());
  std::unordered_set<GameId> seen;
  seen.reserve(games_value->array().size());
  for (const auto& item : games_value->array()) {
    if (cancel.load()) return Failure(LibraryError::kCanceled);
    if (!item.is_object()) return Failure(LibraryError::kProtocol);
    const auto* id_value = item.Find("appid");
    const auto id = id_value ? id_value->AsUint64() : std::nullopt;
    if (!id || !*id || *id > std::numeric_limits<GameId>::max())
      return Failure(LibraryError::kProtocol);

    const auto game_id = static_cast<GameId>(*id);
    GameSummary game;
    game.id = game_id;
    const auto* title = item.Find("name");
    if (!title || (title->is_string() && title->text().empty())) {
      game.title = "app_" + std::to_string(game_id);
      result.metadata_partial = true;
    } else if (!title->is_string() || title->text().size() > kTitleLimit) {
      return Failure(LibraryError::kProtocol);
    } else {
      game.title = title->text();
    }

    const auto* icon = item.Find("img_icon_url");
    if (!icon || (icon->is_string() && icon->text().empty())) {
      result.metadata_partial = true;
    } else if (!icon->is_string() || !IconHashValid(icon->text())) {
      return Failure(LibraryError::kProtocol);
    } else {
      game.artwork_source =
          "https://media.steampowered.com/steamcommunity/public/images/apps/" +
          std::to_string(game_id) + "/" + icon->text() + ".jpg";
    }

    const bool playtime_present = item.Find("playtime_forever") != nullptr;
    const bool last_played_present = item.Find("rtime_last_played") != nullptr;
    if (!ParsePlaytime(item.Find("playtime_forever"), &game.playtime_minutes) ||
        !ParseLastPlayed(item.Find("rtime_last_played"),
                         &game.last_played_unix_seconds)) {
      return Failure(LibraryError::kProtocol);
    }
    if (!playtime_present || !last_played_present) result.metadata_partial = true;

    if (!seen.insert(game_id).second) {
      result.metadata_partial = true;
      continue;
    }
    result.games.push_back(std::move(game));
  }
  if (cancel.load()) return Failure(LibraryError::kCanceled);
  return result;
}

}  // namespace z3cloudplay::steam
