#include "steam/backend/ws_client.h"

#include "steam/backend/native_network.h"

#include <curl/curl.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace z3cloudplay::steam {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kMaxConnectTimeoutMs = 10'000;

struct CurlGlobal {
  CurlGlobal() : ready(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {}
  ~CurlGlobal() {
    if (ready) curl_global_cleanup();
  }
  bool ready;
};

bool CurlReady() {
  static CurlGlobal global;
  return global.ready;
}

bool ValidWebSocketUrl(const std::string& url) {
  if (url.empty() || url.size() > 2048 || url.find('\0') != std::string::npos) return false;
  CURLU* parsed = curl_url();
  if (!parsed) return false;
  char* scheme = nullptr;
  char* host = nullptr;
  const bool valid = curl_url_set(parsed, CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER) ==
                         CURLUE_OK &&
                     curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                     curl_url_get(parsed, CURLUPART_HOST, &host, 0) == CURLUE_OK && scheme &&
                     std::strcmp(scheme, "wss") == 0 && host && *host;
  curl_free(scheme);
  curl_free(host);
  curl_url_cleanup(parsed);
  return valid;
}

HttpError MapConnectError(CURLcode code) {
  switch (code) {
    case CURLE_OK: return HttpError::kNone;
    case CURLE_OPERATION_TIMEDOUT: return HttpError::kTimeout;
    case CURLE_COULDNT_RESOLVE_HOST: return HttpError::kNameResolutionFailed;
    case CURLE_COULDNT_CONNECT: return HttpError::kConnectionFailed;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE: return HttpError::kTlsVerificationFailed;
    default: return HttpError::kProtocolError;
  }
}

// Aborts a blocking connect when the caller cancels or the deadline passes.
struct ConnectAbort {
  const std::atomic<bool>* cancel = nullptr;
  Clock::time_point deadline;
};

int ConnectProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto& abort = *static_cast<const ConnectAbort*>(context);
  return abort.cancel->load(std::memory_order_acquire) || Clock::now() >= abort.deadline ? 1 : 0;
}

std::uint32_t RemainingMs(Clock::time_point deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return remaining <= 0 ? 0 : static_cast<std::uint32_t>(remaining);
}

}  // namespace

class WebSocketClient::Impl {
 public:
  explicit Impl(std::string ca_bundle) : ca_bundle_(std::move(ca_bundle)) {}
  ~Impl() { Close(); }

  HttpError Connect(const std::string& url, std::uint32_t timeout_ms,
                    const std::atomic<bool>& cancel) {
    Close();
    if (!internal::InitializeNativeNetwork() || !CurlReady() || ca_bundle_.empty())
      return HttpError::kInitializationFailed;
    if (!ValidWebSocketUrl(url) || timeout_ms == 0) return HttpError::kInvalidRequest;
    if (cancel.load(std::memory_order_acquire)) return HttpError::kCanceled;
    timeout_ms = std::min(timeout_ms, kMaxConnectTimeoutMs);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    easy_ = curl_easy_init();
    if (!easy_) return HttpError::kInitializationFailed;
    bool configured =
        curl_easy_setopt(easy_, CURLOPT_URL, url.c_str()) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_CAINFO, ca_bundle_.c_str()) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_PROXY, "") == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_PROTOCOLS_STR, "wss") == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
        curl_easy_setopt(easy_, CURLOPT_CONNECT_ONLY, 2L) == CURLE_OK;
    if (!configured) {
      Close();
      return HttpError::kInitializationFailed;
    }
    const auto resolved =
        internal::AttachNativeResolution(easy_, url, timeout_ms, cancel, &resolve_);
    if (resolved != internal::NativeResolveError::kNone) {
      Close();
      if (resolved == internal::NativeResolveError::kCanceled) return HttpError::kCanceled;
      return resolved == internal::NativeResolveError::kTimeout
                 ? HttpError::kTimeout
                 : HttpError::kNameResolutionFailed;
    }
    if (cancel.load(std::memory_order_acquire)) {
      Close();
      return HttpError::kCanceled;
    }
    // Resolution may have used part of the budget; the handshake gets only what is left and
    // aborts promptly on cancel through the progress callback.
    const auto remaining = RemainingMs(deadline);
    if (remaining == 0) {
      Close();
      return HttpError::kTimeout;
    }
    ConnectAbort abort{&cancel, deadline};
    if (curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(remaining)) != CURLE_OK ||
        curl_easy_setopt(easy_, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining)) != CURLE_OK ||
        curl_easy_setopt(easy_, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(easy_, CURLOPT_XFERINFOFUNCTION, ConnectProgress) != CURLE_OK ||
        curl_easy_setopt(easy_, CURLOPT_XFERINFODATA, &abort) != CURLE_OK) {
      Close();
      return HttpError::kInitializationFailed;
    }
    const CURLcode code = curl_easy_perform(easy_);
    // The abort context lives on this stack frame; never call back into it later.
    curl_easy_setopt(easy_, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(easy_, CURLOPT_XFERINFOFUNCTION, nullptr);
    if (code != CURLE_OK) {
      const bool canceled = cancel.load(std::memory_order_acquire);
      Close();
      if (canceled) return HttpError::kCanceled;
      return code == CURLE_ABORTED_BY_CALLBACK ? HttpError::kTimeout : MapConnectError(code);
    }
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(easy_, CURLINFO_ACTIVESOCKET, &socket) == CURLE_OK) socket_ = socket;
    connected_ = true;
    return HttpError::kNone;
  }

  HttpError Send(const std::vector<std::uint8_t>& message, std::uint32_t timeout_ms,
                 const std::atomic<bool>& cancel) {
    if (!connected_) return HttpError::kConnectionFailed;
    if (message.size() > kMaxWebSocketMessageBytes) return HttpError::kInvalidRequest;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t offset = 0;
    // An empty message is still one (empty) binary frame.
    do {
      if (cancel.load(std::memory_order_acquire)) return Fail(HttpError::kCanceled);
      std::size_t sent = 0;
      const CURLcode code = curl_ws_send(easy_, message.data() + offset, message.size() - offset,
                                         &sent, 0, CURLWS_BINARY);
      offset += sent;
      if (code == CURLE_OK && offset == message.size()) return HttpError::kNone;
      if (code != CURLE_OK && code != CURLE_AGAIN) return Fail(HttpError::kConnectionFailed);
      if (RemainingMs(deadline) == 0) return Fail(HttpError::kTimeout);
      Wait(POLLOUT, std::min<std::uint32_t>(RemainingMs(deadline), 25));
    } while (true);
  }

  HttpError Receive(std::vector<std::uint8_t>* message, std::uint32_t wait_ms,
                    const std::atomic<bool>& cancel) {
    if (!connected_) return HttpError::kConnectionFailed;
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    std::uint8_t buffer[16 * 1024];
    for (;;) {
      if (cancel.load(std::memory_order_acquire)) return Fail(HttpError::kCanceled);
      std::size_t received = 0;
      const curl_ws_frame* meta = nullptr;
      const CURLcode code = curl_ws_recv(easy_, buffer, sizeof(buffer), &received, &meta);
      if (code == CURLE_AGAIN) {
        const auto remaining = RemainingMs(deadline);
        if (remaining == 0) return HttpError::kTimeout;
        Wait(POLLIN, std::min<std::uint32_t>(remaining, 25));
        continue;
      }
      if (code != CURLE_OK || !meta) return Fail(HttpError::kConnectionFailed);
      if (meta->flags & CURLWS_CLOSE) return Fail(HttpError::kConnectionFailed);
      // curl answers pings itself; pongs and text frames carry nothing for the CM protocol.
      if (meta->flags & (CURLWS_PING | CURLWS_PONG | CURLWS_TEXT)) {
        if (meta->flags & CURLWS_TEXT) return Fail(HttpError::kProtocolError);
        continue;
      }
      if (received > kMaxWebSocketMessageBytes - partial_.size())
        return Fail(HttpError::kResponseTooLarge);
      partial_.insert(partial_.end(), buffer, buffer + received);
      if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0) {
        *message = std::move(partial_);
        partial_.clear();
        return HttpError::kNone;
      }
    }
  }

  void Close() {
    connected_ = false;
    socket_ = CURL_SOCKET_BAD;
    partial_.clear();
    if (easy_) curl_easy_cleanup(easy_);
    easy_ = nullptr;
    curl_slist_free_all(resolve_);
    resolve_ = nullptr;
  }

 private:
  HttpError Fail(HttpError error) {
    Close();
    return error;
  }

  void Wait(short events, std::uint32_t wait_ms) {
    if (socket_ != CURL_SOCKET_BAD) {
      pollfd descriptor{};
      descriptor.fd = static_cast<int>(socket_);
      descriptor.events = events;
      if (poll(&descriptor, 1, static_cast<int>(wait_ms)) >= 0) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint32_t>(wait_ms, 10)));
  }

  std::string ca_bundle_;
  CURL* easy_ = nullptr;
  curl_slist* resolve_ = nullptr;
  curl_socket_t socket_ = CURL_SOCKET_BAD;
  bool connected_ = false;
  std::vector<std::uint8_t> partial_;
};

WebSocketClient::WebSocketClient(std::string ca_bundle)
    : impl_(std::make_unique<Impl>(std::move(ca_bundle))) {}
WebSocketClient::~WebSocketClient() = default;

HttpError WebSocketClient::Connect(const std::string& url, std::uint32_t timeout_ms,
                                   const std::atomic<bool>& cancel) {
  return impl_->Connect(url, timeout_ms, cancel);
}
HttpError WebSocketClient::Send(const std::vector<std::uint8_t>& message,
                                std::uint32_t timeout_ms, const std::atomic<bool>& cancel) {
  return impl_->Send(message, timeout_ms, cancel);
}
HttpError WebSocketClient::Receive(std::vector<std::uint8_t>* message, std::uint32_t wait_ms,
                                   const std::atomic<bool>& cancel) {
  return impl_->Receive(message, wait_ms, cancel);
}
void WebSocketClient::Close() { impl_->Close(); }

}  // namespace z3cloudplay::steam
