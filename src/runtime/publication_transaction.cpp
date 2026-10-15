#include "runtime/publication_transaction.h"
namespace playden::runtime {
bool IsSettingsPublicationTransactionId(const std::string& transaction) {
  bool nonzero = false;
  if (transaction.size() != 36) return false;
  for (std::size_t i = 0; i < transaction.size(); ++i) {
    const char c = transaction[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
      nonzero |= c != '0';
    }
  }
  return nonzero;
}
}
