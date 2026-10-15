#include "diagnostics/redaction.h"
#include <cassert>
int main() {
  z3cloudplay::steam::SteamEvent event;
  event.challenge_url="https://s.team/q/private-synthetic-secret";
  event.account_name="private-synthetic-account";
  event.steam_id=123456789;
  event.request_id=7; event.generation=3;
  event.kind=z3cloudplay::steam::SteamEventKind::kFailure;
  event.error=z3cloudplay::steam::AuthError::kProtocol;
  const auto record=z3cloudplay::diagnostics::AuthRecord(event);
  assert(record.find("private-synthetic")==std::string::npos);
  assert(record.find("s.team")==std::string::npos);
  assert(record.find("123456789")==std::string::npos);
  assert(record.find("\"request\":7")!=std::string::npos);
  assert(record.find("\"generation\":3")!=std::string::npos);

  event.kind=z3cloudplay::steam::SteamEventKind::kOfflineSession;
  event.error=z3cloudplay::steam::AuthError::kNetwork;
  const auto offline=z3cloudplay::diagnostics::AuthRecord(event);
  assert(offline.find("\"kind\":\"offline-session\"")!=std::string::npos);
  assert(offline.find("private-synthetic")==std::string::npos);
  assert(offline.find("s.team")==std::string::npos);
  assert(offline.find("123456789")==std::string::npos);
}
