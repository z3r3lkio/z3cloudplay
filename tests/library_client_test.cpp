#include "steam/backend/library_client.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace z3cloudplay;
using namespace z3cloudplay::steam;

namespace {

HttpResponse Json(std::string body, long status = 200) {
  HttpResponse response;
  response.status_code = status;
  response.body.assign(body.begin(), body.end());
  return response;
}

struct Fixture {
  std::atomic<bool> cancel{false};
  HttpResponse response;
  std::vector<HttpRequest> requests;
  bool cancel_on_response = false;

  LibraryClientHooks Hooks() {
    return {[this](const HttpRequest& request, const std::atomic<bool>&) {
      requests.push_back(request);
      if (cancel_on_response) cancel = true;
      return response;
    }};
  }
};

StoredSession Session(std::string token = "synthetic-token") {
  return {76561198000000001ULL, "synthetic-account", "unused-refresh",
          std::move(token), 7};
}

LibraryResult Fetch(Fixture* fixture, std::string body) {
  fixture->response = Json(std::move(body));
  LibraryClient client(fixture->Hooks());
  return client.Fetch(Session(), fixture->cancel);
}

std::string ReadFixture(const std::string& directory, const char* name) {
  std::ifstream file(directory + "/" + name, std::ios::binary);
  assert(file);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void SanitizedFixtures(const std::string& directory) {
  {
    Fixture fixture;
    const auto result = Fetch(&fixture, ReadFixture(directory, "owned-games.json"));
    assert(result.error == LibraryError::kNone && !result.metadata_partial);
    assert(result.games.size() == 2);
    assert(result.games[0].id == 1001 && result.games[0].title == "Synthetic One");
    assert(result.games[0].playtime_minutes == 90);
    assert(result.games[0].last_played_unix_seconds == 1700000000);
    assert(result.games[1].id == 1002 && result.games[1].playtime_minutes == 0);
    assert(!result.games[1].last_played_unix_seconds);
  }
  {
    Fixture fixture;
    const auto result = Fetch(&fixture, ReadFixture(directory, "empty.json"));
    assert(result.error == LibraryError::kNone && result.games.empty());
  }
  {
    Fixture fixture;
    assert(Fetch(&fixture, ReadFixture(directory, "incomplete.json")).error ==
           LibraryError::kProtocol);
  }
}

void RequestAndMapping() {
  Fixture fixture;
  const auto result = Fetch(&fixture,
      R"({"response":{"game_count":2,"games":[)"
      R"({"appid":570,"name":"Dota 2","img_icon_url":"0123456789abcdef0123456789abcdef01234567","playtime_forever":123,"rtime_last_played":1700000000},)"
      R"({"appid":10,"name":"Caf\u00e9"}]}})");
  assert(result.error == LibraryError::kNone);
  assert(result.games.size() == 2);
  assert(result.games[0].id == 570 && result.games[0].title == "Dota 2");
  assert(result.games[0].artwork_source ==
      "https://media.steampowered.com/steamcommunity/public/images/apps/570/"
      "0123456789abcdef0123456789abcdef01234567.jpg");
  assert(result.games[0].playtime_minutes == 123);
  assert(result.games[0].last_played_unix_seconds == 1700000000);
  assert(result.games[1].id == 10 && result.games[1].title == "Caf\xc3\xa9");
  assert(result.games[1].artwork_source.empty());
  assert(!result.games[1].playtime_minutes && !result.games[1].last_played_unix_seconds);
  assert(result.metadata_partial);
  assert(fixture.requests.size() == 1);
  const auto& request = fixture.requests.front();
  assert(request.method == HttpMethod::kGet);
  assert(request.url ==
      "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"
      "?access_token=synthetic-token&steamid=76561198000000001&include_appinfo=true&"
      "include_played_free_games=true&format=json");
  const std::vector<std::pair<std::string, std::string>> expected_headers = {
      {"User-Agent", "Z3CloudPlay/1.0"}};
  assert(request.headers == expected_headers);
  assert(request.body.empty());
  assert(request.timeout_ms == 30000);
  assert(request.max_response_bytes == 16 * 1024 * 1024);
}

void PlaytimeAndLastPlayedValidation() {
  {
    Fixture fixture;
    const auto result = Fetch(&fixture,
        R"({"response":{"game_count":1,"games":[{"appid":1,"name":"One","playtime_forever":-7,"rtime_last_played":0}]}})");
    assert(result.error == LibraryError::kNone);
    assert(result.games[0].playtime_minutes == 0);
    assert(!result.games[0].last_played_unix_seconds);
  }
  {
    Fixture fixture;
    const auto result = Fetch(&fixture,
        R"({"response":{"game_count":2,"games":[{"appid":1,"name":"One","playtime_forever":18446744073709551615,"rtime_last_played":9223372036854775807},{"appid":2,"name":"Two","playtime_forever":-9223372036854775808,"rtime_last_played":0}]}})");
    assert(result.error == LibraryError::kNone);
    assert(result.games[0].playtime_minutes == std::numeric_limits<std::uint64_t>::max());
    assert(result.games[0].last_played_unix_seconds ==
           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    assert(result.games[1].playtime_minutes == 0);
  }
  for (const std::string& fields : {
           std::string(R"("playtime_forever":1.5)"),
           std::string(R"("playtime_forever":-9223372036854775809)"),
           std::string(R"("playtime_forever":18446744073709551616)"),
           std::string(R"("playtime_forever":"3")"),
           std::string(R"("rtime_last_played":-1)"),
           std::string(R"("rtime_last_played":1.5)"),
           std::string(R"("rtime_last_played":9223372036854775808)"),
           std::string(R"("rtime_last_played":18446744073709551616)"),
           std::string(R"("rtime_last_played":"3")")}) {
    Fixture fixture;
    const std::string body =
        "{\"response\":{\"game_count\":1,\"games\":[{\"appid\":1,\"name\":\"One\"," +
        fields + "}]}}";
    assert(Fetch(&fixture, body).error == LibraryError::kProtocol);
  }
}

void MissingMetadataAndStableDedupe() {
  Fixture fixture;
  const auto result = Fetch(&fixture,
      R"({"response":{"game_count":3,"games":[)"
      R"({"appid":7},{"appid":8,"name":"First"},{"appid":7,"name":"Replacement"}]}})");
  assert(result.error == LibraryError::kNone && result.metadata_partial);
  assert(result.games.size() == 2);
  assert(result.games[0].id == 7 && result.games[0].title == "app_7");
  assert(result.games[1].id == 8 && result.games[1].title == "First");
}

void EmptyLibraryIsExplicit() {
  for (const std::string& body : {
           std::string(R"({"response":{"game_count":0}})"),
           std::string(R"({"response":{"game_count":0,"games":[]}})")}) {
    Fixture fixture;
    const auto result = Fetch(&fixture, body);
    assert(result.error == LibraryError::kNone && result.games.empty());
    assert(!result.metadata_partial);
  }
}

void CountIsOptionalWhenGamesArePresent() {
  Fixture fixture;
  const auto result = Fetch(&fixture,
      R"({"response":{"games":[{"appid":42,"name":"Complete","img_icon_url":"0123456789abcdef0123456789abcdef01234567","playtime_forever":0,"rtime_last_played":0}]}})");
  assert(result.error == LibraryError::kNone);
  assert(result.games.size() == 1 && result.games[0].id == 42);
  assert(!result.metadata_partial);
}

void RejectsIncompleteAndMalformedResponses() {
  const std::vector<std::string> invalid = {
      R"({"response":{}})",
      R"({"response":{"game_count":1}})",
      R"({"response":{"game_count":2,"games":[{"appid":1,"name":"One"}]}})",
      R"({"response":{"game_count":1,"games":[]}})",
      R"({"response":{"game_count":0,"games":{},"x":1}})",
      R"({"response":{"game_count":"0","games":[]}})",
      R"({"response":{"game_count":1,"games":[{"appid":0,"name":"Bad"}]}})",
      R"({"response":{"game_count":1,"games":[{"appid":4294967296,"name":"Bad"}]}})",
      R"({"response":{"game_count":1,"games":[{"appid":1.5,"name":"Bad"}]}})",
      R"({"response":{"game_count":1,"games":[{"appid":1,"name":7}]}})",
      R"({"response":{"game_count":2,"games":[{"appid":1,"name":"Good"},{"appid":1,"name":7}]}})",
      R"({"response":{"game_count":1,"games":[{"appid":1,"name":"Ok","img_icon_url":"not-a-hash"}]}})",
      R"({"response":{"game_count":0},"response":{"game_count":0}})",
      R"({"response":)",
  };
  for (const auto& body : invalid) {
    Fixture fixture;
    assert(Fetch(&fixture, body).error == LibraryError::kProtocol);
  }
  {
    std::string invalid_utf8 = "{\"response\":{\"game_count\":1,\"games\":[{\"appid\":1,\"name\":\"";
    invalid_utf8.push_back(static_cast<char>(0xff));
    invalid_utf8 += "\"}]}}";
    Fixture fixture;
    assert(Fetch(&fixture, invalid_utf8).error == LibraryError::kProtocol);
  }
  {
    std::string long_title(4097, 'x');
    Fixture fixture;
    assert(Fetch(&fixture, "{\"response\":{\"game_count\":1,\"games\":[{\"appid\":1,\"name\":\"" +
                               long_title + "\"}]}}")
               .error == LibraryError::kProtocol);
  }
}

void BoundsAreIndependentOfTransport() {
  {
    Fixture fixture;
    std::string body(16 * 1024 * 1024 + 1, ' ');
    assert(Fetch(&fixture, std::move(body)).error == LibraryError::kProtocol);
  }
  {
    std::string body = R"({"response":{"games":[)";
    for (int i = 1; i <= 10001; ++i) {
      if (i != 1) body += ',';
      body += "{\"appid\":" + std::to_string(i) + "}";
    }
    body += "]}}";
    Fixture fixture;
    assert(Fetch(&fixture, std::move(body)).error == LibraryError::kProtocol);
  }
}

void ErrorsCancellationAndSessionValidation() {
  {
    Fixture fixture;
    fixture.cancel = true;
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(Session(), fixture.cancel).error == LibraryError::kCanceled);
    assert(fixture.requests.empty());
  }
  {
    Fixture fixture;
    fixture.response = Json(R"({"response":{"game_count":0}})");
    fixture.cancel_on_response = true;
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(Session(), fixture.cancel).error == LibraryError::kCanceled);
  }
  for (const auto error : {HttpError::kTimeout, HttpError::kConnectionFailed,
                           HttpError::kResponseTooLarge}) {
    Fixture fixture;
    fixture.response.error = error;
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(Session(), fixture.cancel).error == LibraryError::kNetwork);
  }
  for (long status : {401L, 403L}) {
    Fixture fixture;
    fixture.response = Json("", status);
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(Session(), fixture.cancel).error == LibraryError::kCredentialsRevoked);
  }
  for (long status : {429L, 500L, 503L}) {
    Fixture fixture;
    fixture.response = Json("", status);
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(Session(), fixture.cancel).error == LibraryError::kNetwork);
  }
  for (long status : {0L, 204L, 400L, 404L}) {
    Fixture fixture;
    fixture.response = Json("", status);
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(Session(), fixture.cancel).error == LibraryError::kProtocol);
  }
  for (StoredSession session : {StoredSession{}, Session("")}) {
    Fixture fixture;
    LibraryClient client(fixture.Hooks());
    assert(client.Fetch(session, fixture.cancel).error == LibraryError::kNoSession);
    assert(fixture.requests.empty());
  }
}

void EncodesCredentialsWithoutExposingThemElsewhere() {
  Fixture fixture;
  fixture.response = Json(R"({"response":{"game_count":0}})");
  LibraryClient client(fixture.Hooks());
  const auto result = client.Fetch(Session("a b&c?d/e+f=%"), fixture.cancel);
  assert(result.error == LibraryError::kNone);
  assert(fixture.requests.size() == 1);
  assert(fixture.requests[0].url.find("access_token=a%20b%26c%3Fd%2Fe%2Bf%3D%25") !=
         std::string::npos);
  assert(fixture.requests[0].headers.size() == 1 && fixture.requests[0].body.empty());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: library_client_test FIXTURE_DIRECTORY\n";
    return 2;
  }
  SanitizedFixtures(argv[1]);
  RequestAndMapping();
  PlaytimeAndLastPlayedValidation();
  MissingMetadataAndStableDedupe();
  EmptyLibraryIsExplicit();
  CountIsOptionalWhenGamesArePresent();
  RejectsIncompleteAndMalformedResponses();
  BoundsAreIndependentOfTransport();
  ErrorsCancellationAndSessionValidation();
  EncodesCredentialsWithoutExposingThemElsewhere();
  std::cout << "library client tests passed\n";
}
