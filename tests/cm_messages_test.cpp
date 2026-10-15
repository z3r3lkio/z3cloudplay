#include "steam/backend/binary_kv.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/cm_messages.h"

#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace z3cloudplay::steam;

namespace {

std::string g_root;

std::vector<std::uint8_t> Fixture(const std::string& name) {
  std::ifstream in(g_root + "/" + name, std::ios::binary);
  assert(in && "missing fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Matches fixtures/steam/proto/generate_cm_fixtures.py.
constexpr const char* kSeed = "6F1C2B8A-3D4E-4F50-9A6B-7C8D9E0F1A2B";

void RequestsMatchProtocBytes() {
  assert(cm::EncodeHello() == Fixture("hello_request.pb"));
  const auto device = MakeDeviceIdentity(kSeed);
  assert(device.machine_name == "Z3CloudPlay" && device.login_id != 0);
  assert(cm::EncodeLogon("synthetic_user", "synthetic.refresh.token", 0, device) ==
         Fixture("logon_request.pb"));
  assert(cm::EncodePicsPackagesRequest({{100, 7001}, {200, 0}}) == Fixture("pics_request.pb"));
  assert(cm::EncodeHeartBeat().empty());
}

void ResponsesDecode() {
  cm::LogonResponse logon;
  assert(cm::DecodeLogonResponse(Fixture("logon_response.pb"), &logon));
  assert(logon.eresult == cm::kEResultOk && logon.heartbeat_seconds == 12 && logon.cell_id == 7);

  cm::LicenseList list;
  assert(cm::DecodeLicenseList(Fixture("license_list.pb"), &list));
  assert(list.eresult == cm::kEResultOk && list.licenses.size() == 5);
  assert(list.licenses[0].package_id == 100 && list.licenses[0].time_created == 1600000000 &&
         list.licenses[0].owner_id == 4242 && list.licenses[0].access_token == 7001);
  assert(list.licenses[2].flags == 8);
  assert(list.licenses[4].minute_limit == 60 && list.licenses[4].minutes_used == 60);

  cm::PicsResponse part;
  assert(cm::DecodePicsResponse(Fixture("pics_response_part1.pb"), &part));
  assert(part.response_pending && part.packages.size() == 1 && part.packages[0].package_id == 100);
  PackageContents contents;
  assert(ParsePackageBuffer(part.packages[0].buffer.data(), part.packages[0].buffer.size(), 100,
                            &contents));
  assert((contents.app_ids == std::vector<std::uint32_t>{10, 20}));
  assert(cm::DecodePicsResponse(Fixture("pics_response_part2.pb"), &part) && !part.response_pending);
}

void LogonResponseFallsBackToLegacyAndDefaultHeartbeat() {
  // eresult 5 only: no heartbeat fields -> 9 seconds.
  cm::LogonResponse logon;
  assert(cm::DecodeLogonResponse({0x08, 0x05}, &logon));
  assert(logon.eresult == cm::kEResultInvalidPassword && logon.heartbeat_seconds == 9);
  // legacy 20 only.
  assert(cm::DecodeLogonResponse({0x08, 0x01, 0x10, 0x14}, &logon) && logon.heartbeat_seconds == 20);
  // eresult with the wrong wire type fails.
  assert(!cm::DecodeLogonResponse({0x0d, 1, 0, 0, 0}, &logon));
}

void FramesRoundTripAndRejectMalformedInput() {
  cm::Header header;
  header.steam_id = cm::kAnonymousSteamId;
  header.session_id = -3;
  header.jobid_source = 9;
  const auto frame = cm::EncodeFrame(cm::kEMsgClientHello, header, {1, 2, 3});
  cm::Frame decoded;
  assert(cm::DecodeFrame(frame.data(), frame.size(), &decoded));
  assert(decoded.emsg == cm::kEMsgClientHello && decoded.header.steam_id == cm::kAnonymousSteamId);
  assert(decoded.header.session_id == -3 && decoded.header.jobid_source == 9u);
  assert(!decoded.header.jobid_target && !decoded.header.eresult);
  assert((decoded.body == std::vector<std::uint8_t>{1, 2, 3}));

  auto plain = frame;
  plain[3] &= 0x7f;  // clear the protobuf flag
  assert(!cm::DecodeFrame(plain.data(), plain.size(), &decoded));
  assert(!cm::DecodeFrame(frame.data(), 7, &decoded));
  auto long_header = frame;
  long_header[4] = 0xff;
  assert(!cm::DecodeFrame(long_header.data(), long_header.size(), &decoded));

  const auto server = Fixture("server_pics_part1.bin");
  assert(cm::DecodeFrame(server.data(), server.size(), &decoded));
  assert(decoded.emsg == cm::kEMsgClientPICSProductInfoResponse && decoded.header.jobid_target == 1u);
  assert(decoded.header.steam_id == 76561197960269970ull && decoded.header.session_id == 77);
}

void TruncatedMessagesFail() {
  auto licenses = Fixture("license_list.pb");
  licenses.resize(licenses.size() - 3);
  cm::LicenseList list;
  assert(!cm::DecodeLicenseList(licenses, &list));

  std::vector<std::vector<std::uint8_t>> frames;
  assert(cm::SplitMultiPayload({2, 0, 0, 0, 7, 8, 1, 0, 0, 0, 9}, &frames) && frames.size() == 2);
  assert(!cm::SplitMultiPayload({2, 0, 0, 0, 7}, &frames));
  assert(!cm::SplitMultiPayload({1, 0, 0}, &frames));
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  g_root = argv[1];
  RequestsMatchProtocBytes();
  ResponsesDecode();
  LogonResponseFallsBackToLegacyAndDefaultHeartbeat();
  FramesRoundTripAndRejectMalformedInput();
  TruncatedMessagesFail();
}
