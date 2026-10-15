#include "steam/secure_session_vault.h"
#include <cassert>
#include <algorithm>
#include <string>
int main() {
  const std::string pass="a-long-independent-test-passphrase";
  const z3cloudplay::Bytes clear={'r','e','f','r','e','s','h','-','t','o','k','e','n'};
  auto one=z3cloudplay::security::SealSession(clear,pass);
  assert(one.ok());
  assert(std::search(one.value().begin(),one.value().end(),clear.begin(),clear.end())==one.value().end());
  auto two=z3cloudplay::security::SealSession(clear,pass);
  assert(two.ok() && one.value()!=two.value());
  auto opened=z3cloudplay::security::OpenSession(one.value(),pass);
  assert(opened.ok() && opened.value()==clear);
  assert(!z3cloudplay::security::OpenSession(one.value(),"wrong-very-long-passphrase").ok());
  auto altered=one.value();
  altered[altered.size()-1]^=1;
  assert(!z3cloudplay::security::OpenSession(altered,pass).ok());
  altered=one.value();
  altered[12]^=1;
  assert(!z3cloudplay::security::OpenSession(altered,pass).ok());
  assert(!z3cloudplay::security::SealSession(clear,"short").ok());
}
