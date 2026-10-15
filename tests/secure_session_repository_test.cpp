#include "steam/secure_session_repository.h"
#include "platform/storage.h"
#include <cassert>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
int main() {
  char root[]="/tmp/z3cloudplay-session-XXXXXX";
  assert(mkdtemp(root));
  const std::string folder=root;
  z3cloudplay::Storage disk(folder);
  const std::string key="test-only-strong-phrase-no-real-tokens";
  z3cloudplay::security::SecureSessionRepository vault(disk,[&]{return key;});
  z3cloudplay::StoredSession input{76561198012345678ULL,"dummy","sample-refresh-secret","sample-access-secret",42};
  assert(vault.Save(input).ok());
  std::ifstream file(folder+"/session.v2",std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)),{});
  assert(bytes.find(input.refresh_token)==std::string::npos);
  assert(bytes.find(input.access_token)==std::string::npos);
  auto loaded=vault.Load();
  assert(loaded.ok() && loaded.value().has_value());
  assert(loaded.value()->refresh_token==input.refresh_token);
  assert(loaded.value()->access_token==input.access_token);
  z3cloudplay::security::SecureSessionRepository wrong(disk,[]{return "incorrect-passphrase-is-long";});
  assert(!wrong.Load().ok());
  assert(vault.Clear().ok());
  assert(vault.Load().ok() && !vault.Load().value().has_value());
  assert(!vault.Save({}).ok());
  rmdir(folder.c_str());
}
