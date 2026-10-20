#include "runtime/payload_update_preflight.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <poll.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

extern char** environ;

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail {
namespace runtime {
namespace {

constexpr int kUpdaterProgressDescriptor = 10;
constexpr char kProgressFinishedPacket = 'S';

// Opening the progress window is a GTK application start: ~200 ms of fork,
// exec, and libadwaita initialisation. The updater it exists for takes about
// 13 ms when everything is warm, which is every launch but the ones after a
// Roblox release. Spawning the window up front therefore charged each of those
// launches for a window that never became visible.
//
// The channel is created with the updater and the window is spawned only once
// the updater has been running this long, so a real download still gets its
// progress window -- it just appears a fraction of a second later, which is
// before there is anything to read. 0 restores the old eager behaviour, and
// MOCKTAIL_DISABLE_UPDATE_PROGRESS still keeps the window away entirely.
constexpr int kDefaultProgressRevealDelayMs = 250;

// Upper bound on one wait step. Bounds how long the loop keeps checking the
// reveal deadline and the child when both ends are quiet.
constexpr int kWaitSliceMs = 20;

int ProgressRevealDelayMs(const Environment& environment) {
  const auto value = environment.Get("MOCKTAIL_UPDATE_PROGRESS_DELAY_MS");
  if (!value.has_value() || value->empty()) {
    return kDefaultProgressRevealDelayMs;
  }
  int parsed = 0;
  const char* begin = value->data();
  const char* end = begin + value->size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end || parsed < 0) {
    return kDefaultProgressRevealDelayMs;
  }
  return parsed;
}

bool Enabled(const Environment& environment, std::string_view name) {
  const auto value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool HasDisplay(const Environment& environment) {
  return environment.HasNonEmpty("DISPLAY") ||
         environment.HasNonEmpty("WAYLAND_DISPLAY");
}

bool IsExecutableRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         !std::filesystem::is_symlink(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

std::filesystem::path UpdateHelper(const Environment& environment) {
  const auto helper_override = environment.Get("MOCKTAIL_UPDATE_HELPER");
  if (helper_override.has_value()) {
    const std::filesystem::path helper(*helper_override);
    if (helper.is_absolute() && IsExecutableRegularFile(helper)) {
      return helper;
    }
    return {};
  }

  const auto project_override = environment.Get("MOCKTAIL_PROJECT_ROOT");
  if (project_override.has_value()) {
    const std::filesystem::path root(*project_override);
    if (!root.is_absolute()) {
      return {};
    }
    for (const std::filesystem::path& helper :
         {root / "build" / "mocktail_updater",
          root / "bin" / "mocktail_updater"}) {
      if (IsExecutableRegularFile(helper)) {
        return helper;
      }
    }
    return {};
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path executable_dir = executable.parent_path();
  const std::vector<std::filesystem::path> candidates = {
      executable_dir / "mocktail_updater",
      executable_dir.parent_path() / "libexec" / "mocktail" /
          "mocktail_updater",
      executable_dir.parent_path() / MOCKTAIL_INSTALL_LIBDIR / "mocktail" /
          "mocktail_updater",
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsExecutableRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path ProgressHelper(const Environment& environment) {
  const auto helper_override =
      environment.Get("MOCKTAIL_UPDATE_PROGRESS_HELPER");
  if (helper_override.has_value()) {
    const std::filesystem::path helper(*helper_override);
    return helper.is_absolute() && IsExecutableRegularFile(helper)
               ? helper
               : std::filesystem::path{};
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path helper =
      executable.parent_path() / "mocktail_failure_dialog";
  return IsExecutableRegularFile(helper) ? helper : std::filesystem::path{};
}

void WaitForProcess(pid_t process) {
  if (process <= 0) {
    return;
  }
  while (waitpid(process, nullptr, 0) < 0 && errno == EINTR) {
  }
}

class UpdateProgressMonitor final {
 public:
  UpdateProgressMonitor() = default;
  ~UpdateProgressMonitor() { Finish(); }

  UpdateProgressMonitor(const UpdateProgressMonitor&) = delete;
  UpdateProgressMonitor& operator=(const UpdateProgressMonitor&) = delete;

  // Creates the packet channel the updater writes to, without spawning
  // anything: the window is opened later by Reveal.
  static UpdateProgressMonitor Prepare(const Environment& environment) {
    if (Enabled(environment, "MOCKTAIL_DISABLE_UPDATE_PROGRESS") ||
        Enabled(environment, "MOCKTAIL_DISABLE_FAILURE_DIALOG") ||
        Enabled(environment, "MOCKTAIL_ISOLATED_CANARY") ||
        Enabled(environment, "MOCKTAIL_HEADLESS") || !HasDisplay(environment)) {
      return {};
    }
    const std::filesystem::path helper = ProgressHelper(environment);
    if (helper.empty()) {
      return {};
    }

    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
      return {};
    }
    return UpdateProgressMonitor(sockets[0], sockets[1], std::move(helper));
  }

  bool active() const { return socket_ >= 0; }
  bool revealed() const { return process_ > 0; }
  // The descriptor the updater writes its progress packets to. It is the same
  // end this process writes the final packet to, which is why the window sees
  // both.
  //
  // While no window exists the packets queue in the socket. That is bounded by
  // the socket buffer, and the updater sends one small packet per stage rather
  // than per byte, so an update that runs for minutes never comes close.
  int updater_descriptor() const { return socket_; }

  // Spawns the window and hands it the reading end of the channel. Whatever the
  // updater has written so far is already queued there and is read first, so
  // nothing said before this point is lost. A spawn failure costs the window
  // and nothing else: the updater keeps its descriptor and finishes as usual.
  void Reveal() {
    if (!active() || revealed()) {
      return;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
      return;
    }
    int action_status = 0;
    if (monitor_socket_ == STDIN_FILENO) {
      action_status =
          posix_spawn_file_actions_addclose(&actions, monitor_socket_);
    }
    if (action_status == 0 && monitor_socket_ != STDIN_FILENO) {
      action_status = posix_spawn_file_actions_adddup2(
          &actions, monitor_socket_, STDIN_FILENO);
    }

    std::string helper_string = helper_.string();
    char progress_option[] = "--progress-monitor";
    char* arguments[] = {helper_string.data(), progress_option, nullptr};
    pid_t child = -1;
    int spawn_status = action_status;
    if (spawn_status == 0) {
      spawn_status = posix_spawn(&child, helper_.c_str(), &actions, nullptr,
                                 arguments, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_status != 0) {
      return;
    }
    // The window owns the reading end now.
    close(monitor_socket_);
    monitor_socket_ = -1;
    process_ = child;
  }

 private:
  UpdateProgressMonitor(int updater_socket, int monitor_socket,
                         std::filesystem::path helper)
      : socket_(updater_socket), monitor_socket_(monitor_socket),
        helper_(std::move(helper)) {}

  void Finish() {
    if (monitor_socket_ >= 0) {
      // No window was ever opened: nothing is reading, so nothing to signal.
      close(monitor_socket_);
      monitor_socket_ = -1;
    }
    if (!active()) {
      return;
    }
    if (process_ > 0) {
      (void)send(socket_, &kProgressFinishedPacket,
                 sizeof(kProgressFinishedPacket), MSG_NOSIGNAL);
      (void)shutdown(socket_, SHUT_RDWR);
    }
    close(socket_);
    socket_ = -1;
    WaitForProcess(process_);
    process_ = -1;
  }

  int socket_ = -1;
  int monitor_socket_ = -1;
  pid_t process_ = -1;
  std::filesystem::path helper_;
};

bool IsEnvironmentEntry(std::string_view entry, std::string_view name) {
  return entry.size() > name.size() && entry[name.size()] == '=' &&
         entry.substr(0, name.size()) == name;
}

bool IsOverriddenEnvironmentEntry(
    std::string_view entry,
    const std::vector<std::pair<std::string_view, std::filesystem::path>>&
        overrides) {
  for (const auto& [name, ignored] : overrides) {
    if (IsEnvironmentEntry(entry, name)) {
      return true;
    }
  }
  return false;
}

// The updater runs as a separate process, so the reason it failed ("api.
// pureapk.com request returned status 503") only ever reached the session log.
// Relay its stderr verbatim and keep the last failure it reported, so the
// launcher can tell the user what actually went wrong.
class UpdaterStderrRelay final {
 public:
  UpdaterStderrRelay() {
    if (pipe2(descriptors_, O_CLOEXEC) != 0) {
      descriptors_[0] = -1;
      descriptors_[1] = -1;
    }
  }

  ~UpdaterStderrRelay() {
    CloseDescriptor(&descriptors_[0]);
    CloseWrite();
  }

  UpdaterStderrRelay(const UpdaterStderrRelay&) = delete;
  UpdaterStderrRelay& operator=(const UpdaterStderrRelay&) = delete;

  bool active() const { return descriptors_[0] >= 0; }
  int write_descriptor() const { return descriptors_[1]; }
  void CloseWrite() { CloseDescriptor(&descriptors_[1]); }
  const std::string& failure() const { return failure_; }

  // Must run before waitpid: a child that fills the pipe blocks until it is
  // drained, and waiting first would deadlock the launcher.
  void Drain() {
    if (!active()) {
      return;
    }
    while (ConsumeAvailable(kWaitSliceMs)) {
    }
    RecordLine(pending_);
    pending_.clear();
    eof_ = true;
  }

  // Reads whatever is already there, waiting up to `timeout_ms` for the first
  // byte. False means end-of-file or nothing arrived within the timeout; use
  // eof() to tell those apart. Complete lines are recorded as they arrive, so
  // this is safe to call repeatedly while the child is still running.
  bool ConsumeAvailable(int timeout_ms) {
    if (!active() || eof_) {
      return false;
    }
    pollfd watched = {};
    watched.fd = descriptors_[0];
    watched.events = POLLIN;
    const int ready = poll(&watched, 1, timeout_ms);
    if (ready < 0) {
      return errno == EINTR;
    }
    if (ready == 0) {
      return false;
    }
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t received =
          read(descriptors_[0], buffer.data(), buffer.size());
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received < 0) {
        return false;
      }
      if (received == 0) {
        RecordLine(pending_);
        pending_.clear();
        eof_ = true;
        CloseDescriptor(&descriptors_[0]);
        return false;
      }
      const auto bytes = static_cast<std::size_t>(received);
      WriteAll(buffer.data(), bytes);
      pending_.append(buffer.data(), bytes);
      for (std::size_t end = pending_.find('\n'); end != std::string::npos;
           end = pending_.find('\n')) {
        RecordLine(std::string_view(pending_).substr(0, end));
        pending_.erase(0, end + 1);
      }
      // A blocking descriptor would park here on the next read; poll says when
      // there is more.
      pollfd again = {};
      again.fd = descriptors_[0];
      again.events = POLLIN;
      if (poll(&again, 1, 0) <= 0) {
        return true;
      }
    }
  }

  bool eof() const { return eof_; }

 private:
  // An Adwaita alert dialog stays readable well below the packet limit.
  static constexpr std::size_t kMaximumFailureBytes = 400;

  static void CloseDescriptor(int* descriptor) {
    if (*descriptor >= 0) {
      close(*descriptor);
      *descriptor = -1;
    }
  }

  static void WriteAll(const char* data, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
      const ssize_t written =
          write(STDERR_FILENO, data + offset, bytes - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        return;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  // A message cut mid-codepoint fails UTF-8 validation in the dialog helper
  // and would be replaced by the generic text.
  static std::string_view TrimToCharacterBoundary(std::string_view text) {
    if (text.size() <= kMaximumFailureBytes) {
      return text;
    }
    std::size_t size = kMaximumFailureBytes;
    while (size > 0 &&
           (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
      --size;
    }
    return text.substr(0, size);
  }

  void RecordLine(std::string_view line) {
    constexpr std::string_view kPrefix = "[native-updater] ";
    constexpr std::string_view kWarningPrefix = "warning: ";
    if (line.size() <= kPrefix.size() ||
        line.substr(0, kPrefix.size()) != kPrefix) {
      return;
    }
    const std::string_view detail = line.substr(kPrefix.size());
    if (detail.substr(0, kWarningPrefix.size()) == kWarningPrefix) {
      return;
    }
    failure_.assign(TrimToCharacterBoundary(detail));
  }

  int descriptors_[2] = {-1, -1};
  bool eof_ = false;
  std::string pending_;
  std::string failure_;
};

std::vector<std::string> ChildEnvironment(const RuntimePaths& paths,
                                          bool progress_enabled) {
  const std::vector<std::pair<std::string_view, std::filesystem::path>>
      overrides = {
          {"MOCKTAIL_CONFIG_FILE", paths.config_file()},
          {"MOCKTAIL_CONFIG_ROOT", paths.config_root()},
          {"MOCKTAIL_DATA_ROOT", paths.data_root()},
          {"MOCKTAIL_CACHE_ROOT", paths.cache_root()},
          {"MOCKTAIL_STATE_ROOT", paths.state_root()},
      };
  std::vector<std::string> child_environment;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (!IsOverriddenEnvironmentEntry(*entry, overrides) &&
        !IsEnvironmentEntry(*entry, "MOCKTAIL_UPDATE_PROGRESS_FD")) {
      child_environment.emplace_back(*entry);
    }
  }
  for (const auto& [name, value] : overrides) {
    child_environment.emplace_back(std::string(name) + "=" + value.string());
  }
  if (progress_enabled) {
    child_environment.emplace_back("MOCKTAIL_UPDATE_PROGRESS_FD=" +
                                   std::to_string(kUpdaterProgressDescriptor));
  }
  return child_environment;
}

}  // namespace

PayloadUpdatePreflightResult RunPayloadUpdatePreflight(
    const Environment& environment, const RuntimePaths& paths,
    bool force_run_latest) {
  PayloadUpdatePreflightResult result;
  if (!force_run_latest &&
      (Enabled(environment, "MOCKTAIL_SKIP_UPDATE_CHECK") ||
       environment.HasNonEmpty("ROBLOX_LIB_PATH"))) {
    return result;
  }

  const std::filesystem::path helper = UpdateHelper(environment);
  if (helper.empty()) {
    if (force_run_latest ||
        environment.HasNonEmpty("MOCKTAIL_RUNTIME_LIBRARY_DIR")) {
      result.error = "cannot locate the Mocktail update helper";
    }
    return result;
  }

  result.attempted = true;
  std::string helper_string = helper.string();
  char update[] = "update";
  char startup_preflight[] = "--startup-preflight";
  char force_latest[] = "--force-run-latest";
  char* arguments[] = {helper_string.data(), update,
                       force_run_latest ? force_latest : startup_preflight,
                       nullptr};
  UpdateProgressMonitor progress = UpdateProgressMonitor::Prepare(environment);
  const int reveal_delay_ms = ProgressRevealDelayMs(environment);
  std::vector<std::string> child_environment =
      ChildEnvironment(paths, progress.active());
  std::vector<char*> child_environment_pointers;
  child_environment_pointers.reserve(child_environment.size() + 1);
  for (std::string& entry : child_environment) {
    child_environment_pointers.push_back(entry.data());
  }
  child_environment_pointers.push_back(nullptr);
  UpdaterStderrRelay stderr_relay;
  posix_spawn_file_actions_t actions;
  bool actions_initialized = false;
  int action_status = 0;
  if (progress.active() || stderr_relay.active()) {
    action_status = posix_spawn_file_actions_init(&actions);
    actions_initialized = action_status == 0;
  }
  if (action_status == 0 && progress.active()) {
    action_status = posix_spawn_file_actions_adddup2(
        &actions, progress.updater_descriptor(), kUpdaterProgressDescriptor);
  }
  if (action_status == 0 && stderr_relay.active()) {
    action_status = posix_spawn_file_actions_adddup2(
        &actions, stderr_relay.write_descriptor(), STDERR_FILENO);
  }
  if (action_status != 0) {
    if (actions_initialized) {
      posix_spawn_file_actions_destroy(&actions);
    }
    result.error = "cannot prepare Roblox update progress channel: " +
                   std::string(std::strerror(action_status));
    return result;
  }
  pid_t child = -1;
  const int spawn_status = posix_spawn(
      &child, helper.c_str(), actions_initialized ? &actions : nullptr, nullptr,
      arguments, child_environment_pointers.data());
  if (actions_initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  if (spawn_status != 0) {
    result.error = "cannot start Roblox update preflight: " +
                   std::string(std::strerror(spawn_status));
    return result;
  }

  // The child owns the write end now; the relay must observe end-of-file.
  stderr_relay.CloseWrite();

  // Wait for the updater while draining its stderr, and open the progress
  // window only if it is still running once the reveal delay is up. A warm
  // preflight exits inside the delay and the window is never created, which is
  // where the GTK start-up cost went.
  int child_status = 0;
  bool child_finished = false;
  const auto reveal_at = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(reveal_delay_ms);
  while (true) {
    const pid_t reaped = waitpid(child, &child_status, WNOHANG);
    if (reaped == child) {
      child_finished = true;
      break;
    }
    if (reaped < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = "cannot wait for Roblox update preflight: " +
                     std::string(std::strerror(errno));
      return result;
    }
    if (progress.active() && !progress.revealed() &&
        std::chrono::steady_clock::now() >= reveal_at) {
      progress.Reveal();
    }
    if (stderr_relay.eof()) {
      // Every writer closed the pipe; only the exit status is left to collect.
      poll(nullptr, 0, kWaitSliceMs);
      continue;
    }
    stderr_relay.ConsumeAvailable(kWaitSliceMs);
  }
  stderr_relay.Drain();

  while (!child_finished) {
    if (waitpid(child, &child_status, 0) >= 0) {
      break;
    }
    if (errno != EINTR) {
      result.error = "cannot wait for Roblox update preflight: " +
                     std::string(std::strerror(errno));
      return result;
    }
  }
  if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    result.error = force_run_latest ? "forced latest Roblox run failed"
                                     : "Roblox update preflight failed";
    result.details = stderr_relay.failure();
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
