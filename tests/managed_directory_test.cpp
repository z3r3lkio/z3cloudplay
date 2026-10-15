#include "platform/managed_directory.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s (errno %d)\n", __LINE__, #x, errno); std::exit(1); } } while (0)
namespace fs = std::filesystem;
using z3cloudplay::platform::ManagedDirectory;
using z3cloudplay::platform::DirectoryBackend;
using z3cloudplay::platform::DirectoryPolicy;

namespace {
struct Fixture {
  fs::path root;
  Fixture() {
    char pattern[] = "/tmp/z3cloudplay-managed-directory-XXXXXX";
    CHECK(::mkdtemp(pattern));
    root = fs::canonical(pattern);
  }
  ~Fixture() { fs::remove_all(root); }
};

void ReadWriteRenameAndRemove(DirectoryBackend backend) {
  Fixture f;
  CHECK(::chmod(f.root.c_str(), 0755) == 0);
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kApprovedTree, backend);
  CHECK(root && root->Validate() && root->backend() == backend);
  auto private_dir = root->OpenChild("private", DirectoryPolicy::kPrivate, true);
  CHECK(private_dir && private_dir->root() == f.root / "private");
  auto child = private_dir->OpenChild("nested", DirectoryPolicy::kPrivate, true);
  CHECK(child && child->root() == private_dir->root());
  CHECK(!private_dir->OpenChild("public", DirectoryPolicy::kApprovedTree, true));
  struct stat info{};
  CHECK(::fstat(child->fd(), &info) == 0 && info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0700);
  const int file = child->CreateExclusiveFile("temporary", 0600);
  CHECK(file >= 0);
  CHECK(::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0600);
  CHECK(::write(file, "hello", 5) == 5 && ::fsync(file) == 0 && ::close(file) == 0);
  CHECK(child->CreateExclusiveFile("temporary", 0600) < 0 && errno == EEXIST);
  CHECK(child->RenameChild("temporary", *private_dir, "record"));
  CHECK(child->Sync() && private_dir->Sync());
  int input = private_dir->OpenRegularFile("record", true, 0600);
  char bytes[6]{};
  CHECK(input >= 0 && ::read(input, bytes, sizeof(bytes)) == 5 && std::string(bytes) == "hello");
  CHECK(::close(input) == 0);
  CHECK(private_dir->OpenRegularFile("record", true, 0644) < 0);
  CHECK(private_dir->InspectChild("record", &info) && S_ISREG(info.st_mode));
  CHECK(!private_dir->RemoveEmptyDirectory("record"));
  CHECK(!private_dir->RemoveRegularFile("nested"));
  CHECK(private_dir->RemoveRegularFile("record") && private_dir->RemoveEmptyDirectory("nested") && private_dir->Sync());
  CHECK(!fs::exists(f.root / "private/record"));
  auto moved = std::move(*private_dir);
  CHECK(moved.Validate() && !private_dir->Validate());
}

void RejectTraversalLinksAndSpecials(DirectoryBackend backend) {
  Fixture f;
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(root);
  for (const std::string& name : std::vector<std::string>{"", ".", "..", "../outside", "a/b", "/absolute", "a\\b", std::string("x\0y", 3)}) {
    CHECK(!root->OpenChild(name, DirectoryPolicy::kPrivate, true));
    CHECK(root->CreateExclusiveFile(name, 0600) < 0);
    CHECK(root->OpenRegularFile(name) < 0);
    CHECK(!root->RemoveRegularFile(name));
  }
  CHECK(!ManagedDirectory::Open("relative", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(f.root / "../other", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(f.root.string() + "//real", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(f.root.string() + "/", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(fs::path(f.root.string() + std::string("/a\0b", 4)), DirectoryPolicy::kApprovedTree, backend));
  CHECK(root->CreateExclusiveFile(std::string(8192, 'x'), 0600) < 0 && errno == ENAMETOOLONG);
  CHECK(::mkfifo((f.root / "fifo").c_str(), 0600) == 0);
  CHECK(root->OpenRegularFile("fifo") < 0);
  struct stat info{};
  CHECK(!root->InspectChild("fifo", &info) && !root->RemoveRegularFile("fifo"));
  fs::create_directory(f.root / "real");
  fs::create_directory_symlink("real", f.root / "linked");
  fs::create_symlink("fifo", f.root / "file-link");
  CHECK(!ManagedDirectory::Open(f.root / "linked", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!root->OpenChild("linked", DirectoryPolicy::kApprovedTree));
  CHECK(root->OpenRegularFile("file-link") < 0 && !root->RemoveRegularFile("file-link"));
  CHECK(root->CreateExclusiveFile("bad-mode", 04700) < 0);
  CHECK(!fs::exists(f.root / "bad-mode"));
}

void RejectPolicyAndIdentityDrift(DirectoryBackend backend, bool replacement_link) {
  Fixture f;
  fs::create_directory(f.root / "parent");
  CHECK(::chmod((f.root / "parent").c_str(), 0755) == 0);
  CHECK(!ManagedDirectory::Open(f.root / "parent", DirectoryPolicy::kPrivate, backend));
  auto parent = ManagedDirectory::Open(f.root / "parent", DirectoryPolicy::kApprovedTree, backend);
  CHECK(parent);
  auto child = parent->OpenChild("child", DirectoryPolicy::kPrivate, true);
  CHECK(child);
  CHECK(::chmod(child->path().c_str(), 0755) == 0);
  CHECK(!child->Validate() && child->CreateExclusiveFile("forbidden", 0600) < 0);
  CHECK(::chmod(child->path().c_str(), 0700) == 0);
  CHECK(child->Validate());
  CHECK(::chmod(parent->path().c_str(), 0700) == 0);
  CHECK(!child->Validate());
  CHECK(::chmod(parent->path().c_str(), 0755) == 0);
  fs::rename(f.root / "parent", f.root / "original");
  fs::create_directory(f.root / "replacement");
  if (replacement_link) fs::create_directory_symlink("replacement", f.root / "parent");
  else fs::rename(f.root / "replacement", f.root / "parent");
  CHECK(!parent->Validate() && !child->Validate());
  CHECK(parent->CreateExclusiveFile("forbidden", 0600) < 0);
  CHECK(!parent->OpenChild("forbidden", DirectoryPolicy::kPrivate, true));
  CHECK(!parent->Sync());
  CHECK(fs::is_empty(replacement_link ? f.root / "replacement" : f.root / "parent"));
  CHECK(fs::is_empty(f.root / "original/child"));
}

void RenameAdmission(DirectoryBackend backend) {
  Fixture f;
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(root);
  auto stage = root->OpenChild("staged", DirectoryPolicy::kPrivate, true);
  CHECK(stage && root->RenameChild("staged", *root, "published", false));
  CHECK(!stage->Validate());
  auto reopened = root->OpenChild("published", DirectoryPolicy::kPrivate);
  CHECK(reopened);
  for (const char* name : {"source", "existing"}) {
    const int fd = root->CreateExclusiveFile(name, 0600);
    CHECK(fd >= 0 && ::close(fd) == 0);
  }
  CHECK(!root->RenameChild("source", *root, "existing", false) && errno == EEXIST);
  CHECK(fs::exists(f.root / "source"));
  CHECK(!root->RenameChild("source", *root, "published"));
  CHECK(root->RenameChild("source", *root, "existing"));
  auto other_backend = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate,
      backend == DirectoryBackend::kAbsolutePath ? DirectoryBackend::kDescriptorRelative : DirectoryBackend::kAbsolutePath);
#if defined(__PROSPERO__) || (defined(Z3CLOUDPLAY_PLATFORM_PS5) && Z3CLOUDPLAY_PLATFORM_PS5)
  CHECK(!other_backend && errno == ENOTSUP);
#else
  CHECK(other_backend && !root->RenameChild("existing", *other_backend, "other"));
#endif
  CHECK(fs::exists(f.root / "existing") && !fs::exists(f.root / "other"));
  auto removed = root->OpenChild("removed", DirectoryPolicy::kPrivate, true);
  CHECK(removed);
  fs::rename(f.root / "removed", f.root / "moved");
  fs::create_directory(f.root / "removed");
  CHECK(!root->RenameChild("existing", *removed, "record"));
  CHECK(fs::exists(f.root / "existing") && fs::is_empty(f.root / "removed"));
}

void ForeignOwnershipWhenPrivileged(DirectoryBackend backend) {
  if (::geteuid() != 0) return;  // Exercise in privileged CI; no credential changes.
  Fixture f;
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(root);
  const int file = root->CreateExclusiveFile("foreign", 0600);
  CHECK(file >= 0 && ::fchown(file, 1, static_cast<gid_t>(-1)) == 0 && ::close(file) == 0);
  CHECK(root->OpenRegularFile("foreign", true) < 0);
  CHECK(!root->RemoveRegularFile("foreign"));
  CHECK(!root->RenameChild("foreign", *root, "renamed"));
  CHECK(fs::exists(f.root / "foreign"));
  fs::create_directory(f.root / "foreign-dir");
  CHECK(::chmod((f.root / "foreign-dir").c_str(), 0700) == 0);
  CHECK(::chown((f.root / "foreign-dir").c_str(), 1, static_cast<gid_t>(-1)) == 0);
  CHECK(!root->OpenChild("foreign-dir", DirectoryPolicy::kPrivate));
  auto approved = ManagedDirectory::Open(f.root / "foreign-dir", DirectoryPolicy::kApprovedTree, backend);
  CHECK(approved); // Read-only kit roots may belong to another user.
  CHECK(::chown((f.root / "foreign-dir").c_str(), 0, static_cast<gid_t>(-1)) == 0);
  CHECK(!approved->Validate());
}
}

// kUnownedTree is used for game folders on drives without owners or permissions.
// The host filesystem keeps modes, so this checks that the policy's operations
// work and that links and specials are still refused; the probe sees ownership.
void UnownedTreeOperations(DirectoryBackend backend) {
  Fixture f;
  CHECK(::chmod(f.root.c_str(), 0777) == 0);
  CHECK(z3cloudplay::platform::RecordsOwnership(f.root) == std::optional<bool>(true));
  CHECK(!fs::exists(f.root / (".z3cloudplay-ownership-probe-" + std::to_string(::getpid()))));
  CHECK(!z3cloudplay::platform::RecordsOwnership(f.root / "missing"));
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kUnownedTree, backend);
  CHECK(root && root->policy() == DirectoryPolicy::kUnownedTree);
  const int file = root->CreateExclusiveFile(".dxvk.conf.tmp", 0644);
  CHECK(file >= 0 && ::write(file, "x", 1) == 1 && ::close(file) == 0);
  { std::ofstream existing(f.root / "dxvk.conf"); existing << "old"; }
  CHECK(::chmod((f.root / "dxvk.conf").c_str(), 0666) == 0);
  CHECK(root->RenameChild(".dxvk.conf.tmp", *root, "dxvk.conf", true));
  const int read = root->OpenRegularFile("dxvk.conf");
  char byte = 0;
  CHECK(read >= 0 && ::read(read, &byte, 1) == 1 && byte == 'x' && ::close(read) == 0);
  fs::create_symlink("dxvk.conf", f.root / "linked.conf");
  CHECK(root->OpenRegularFile("linked.conf") < 0 && !root->RemoveRegularFile("linked.conf"));
  CHECK(root->RemoveRegularFile("dxvk.conf") && !fs::exists(f.root / "dxvk.conf"));
}

int main() {
#if defined(__PROSPERO__) || (defined(Z3CLOUDPLAY_PLATFORM_PS5) && Z3CLOUDPLAY_PLATFORM_PS5)
  CHECK(ManagedDirectory::DefaultBackend() == DirectoryBackend::kAbsolutePath);
  const std::vector<DirectoryBackend> backends{DirectoryBackend::kAbsolutePath};
#else
  CHECK(ManagedDirectory::DefaultBackend() == DirectoryBackend::kDescriptorRelative);
  const std::vector<DirectoryBackend> backends{DirectoryBackend::kDescriptorRelative, DirectoryBackend::kAbsolutePath};
#endif
  for (auto backend : backends) {
    ReadWriteRenameAndRemove(backend);
    RejectTraversalLinksAndSpecials(backend);
    RejectPolicyAndIdentityDrift(backend, false);
    RejectPolicyAndIdentityDrift(backend, true);
    RenameAdmission(backend);
    ForeignOwnershipWhenPrivileged(backend);
    UnownedTreeOperations(backend);
  }
  std::printf("managed directory: %zu backends, identity, policy and file operations pass\n", backends.size());
}
