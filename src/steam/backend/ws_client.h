#pragma once

#include "steam/backend/http_client.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace z3cloudplay::steam {

inline constexpr std::size_t kMaxWebSocketMessageBytes = 8 * 1024 * 1024;

// Abstract binary-message transport so the CM client can be tested without a network.
class MessageTransport {
 public:
  virtual ~MessageTransport() = default;
  virtual HttpError Connect(const std::string& url, std::uint32_t timeout_ms,
                            const std::atomic<bool>& cancel) = 0;
  virtual HttpError Send(const std::vector<std::uint8_t>& message, std::uint32_t timeout_ms,
                         const std::atomic<bool>& cancel) = 0;
  // Waits at most wait_ms for one complete binary message. kTimeout only means nothing arrived
  // yet; any other error ends the connection.
  virtual HttpError Receive(std::vector<std::uint8_t>* message, std::uint32_t wait_ms,
                            const std::atomic<bool>& cancel) = 0;
  virtual void Close() = 0;
};

// Verified wss:// client over the pinned libcurl. One worker thread owns each instance; only the
// cancel flag may change from another thread. Errors are stable categories with no URL, address
// or payload text.
class WebSocketClient final : public MessageTransport {
 public:
  explicit WebSocketClient(std::string ca_bundle);
  ~WebSocketClient() override;
  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  HttpError Connect(const std::string& url, std::uint32_t timeout_ms,
                    const std::atomic<bool>& cancel) override;
  HttpError Send(const std::vector<std::uint8_t>& message, std::uint32_t timeout_ms,
                 const std::atomic<bool>& cancel) override;
  HttpError Receive(std::vector<std::uint8_t>* message, std::uint32_t wait_ms,
                    const std::atomic<bool>& cancel) override;
  void Close() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace z3cloudplay::steam
