#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Steam Cloud's unified service messages (Cloud.*#1), a wire-compatible subset of SteamDatabase's
// steammessages_cloud.steamclient.proto. They travel as kEMsgServiceMethodCallFromClient bodies.
namespace z3cloudplay::steam::cloud {

inline constexpr const char* kGetAppFileChangelist = "Cloud.GetAppFileChangelist#1";
inline constexpr const char* kClientFileDownload = "Cloud.ClientFileDownload#1";
inline constexpr const char* kBeginAppUploadBatch = "Cloud.BeginAppUploadBatch#1";
inline constexpr const char* kClientBeginFileUpload = "Cloud.ClientBeginFileUpload#1";
inline constexpr const char* kClientCommitFileUpload = "Cloud.ClientCommitFileUpload#1";
inline constexpr const char* kClientDeleteFile = "Cloud.ClientDeleteFile#1";
inline constexpr const char* kCompleteAppUploadBatchBlocking = "Cloud.CompleteAppUploadBatchBlocking#1";

inline constexpr std::size_t kMaxRemoteFiles = 100'000;
inline constexpr std::uint64_t kMaxRemoteFileBytes = 64ULL * 1024 * 1024;

enum class RemoteState { kPresent, kForgotten, kDeleted };

struct RemoteFile {
  std::string name;  // path prefix applied, e.g. "%WinMyDocuments%My Games/Game/save1.sav"
  std::vector<std::uint8_t> sha1;  // 20 bytes when present
  std::uint64_t time_stamp = 0;
  std::uint64_t raw_size = 0;
  RemoteState state = RemoteState::kPresent;
  bool reupload_requested = false;
};

struct RemoteFileList {
  std::uint64_t change_number = 0;
  std::vector<RemoteFile> files;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

struct DownloadTicket {
  std::uint64_t file_size = 0;      // bytes on the wire (a one-entry ZIP when it differs from raw)
  std::uint64_t raw_file_size = 0;
  std::vector<std::uint8_t> sha1;
  std::uint64_t time_stamp = 0;
  bool explicit_delete = false;
  bool encrypted = false;
  bool use_https = false;
  std::string host;
  std::string path;
  Headers headers;
};

struct UploadBlock {
  std::string host;
  std::string path;
  bool use_https = false;
  int http_method = 0;  // EHTTPMethod: 1 GET, 2 HEAD, 3 POST, 4 PUT, 5 DELETE, 6 OPTIONS, 7 PATCH
  Headers headers;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::optional<std::vector<std::uint8_t>> explicit_body;
};

struct UploadPlan {
  bool encrypt_file = false;
  std::vector<UploadBlock> blocks;
};

struct BatchStart {
  std::uint64_t batch_id = 0;
  std::uint64_t app_change_number = 0;
};

std::vector<std::uint8_t> EncodeChangelistRequest(std::uint32_t app_id, std::uint64_t synced_change_number);
// Fails (nullopt) on a delta list, a missing change number, an unknown prefix index, duplicate or
// invalid names, an unknown persist state, or a present file without its size and SHA-1.
std::optional<RemoteFileList> DecodeChangelistResponse(const std::vector<std::uint8_t>& body);
// Only the app's current change number, from a full or a delta list; for the quick check, which
// compares it with the number recorded at the last sync.
std::optional<std::uint64_t> DecodeChangelistChangeNumber(const std::vector<std::uint8_t>& body);

std::vector<std::uint8_t> EncodeFileDownloadRequest(std::uint32_t app_id, const std::string& name);
std::optional<DownloadTicket> DecodeFileDownloadResponse(const std::vector<std::uint8_t>& body);

std::vector<std::uint8_t> EncodeBeginBatchRequest(std::uint32_t app_id, const std::string& machine_name,
                                                  const std::vector<std::string>& uploads,
                                                  const std::vector<std::string>& deletes,
                                                  std::uint64_t client_id, std::uint64_t app_build_id);
std::optional<BatchStart> DecodeBeginBatchResponse(const std::vector<std::uint8_t>& body);

std::vector<std::uint8_t> EncodeBeginFileUploadRequest(std::uint32_t app_id, std::uint64_t raw_size,
                                                       const std::vector<std::uint8_t>& sha1, std::uint64_t time_stamp,
                                                       const std::string& name, std::uint64_t batch_id);
std::optional<UploadPlan> DecodeBeginFileUploadResponse(const std::vector<std::uint8_t>& body);

std::vector<std::uint8_t> EncodeCommitFileUploadRequest(bool transfer_succeeded, std::uint32_t app_id,
                                                        const std::vector<std::uint8_t>& sha1, const std::string& name);
std::optional<bool> DecodeCommitFileUploadResponse(const std::vector<std::uint8_t>& body);

std::vector<std::uint8_t> EncodeDeleteFileRequest(std::uint32_t app_id, const std::string& name, std::uint64_t batch_id);
std::vector<std::uint8_t> EncodeCompleteBatchRequest(std::uint32_t app_id, std::uint64_t batch_id, bool succeeded);

}  // namespace z3cloudplay::steam::cloud
