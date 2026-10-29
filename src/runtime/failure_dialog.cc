#include "runtime/failure_dialog.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/helper_environment.h"
#include "runtime/helper_program.h"

extern char** environ;

namespace mocktail {
namespace runtime {
namespace {

constexpr char kMessagePacket = 'M';
constexpr char kFailurePacket = 'F';
constexpr char kSuccessPacket = 'S';
constexpr char kConfirmationReadyPacket = 'R';
constexpr char kConfirmationErrorPacket = 'E';
constexpr int kConfirmationReadyFd = 3;
constexpr std::size_t kMaximumMessageBytes = 2048;

bool Enabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool HasNonEmpty(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty();
}

std::filesystem::path DialogHelper(const Environment& environment) {
  return FindHelperProgram(environment, "MOCKTAIL_FAILURE_DIALOG_HELPER",
                           "mocktail_failure_dialog");
}

int WaitForHelper(int helper_pid) {
  if (helper_pid <= 0) {
    return -1;
  }
  int status = 0;
  pid_t result = 0;
  do {
    result = waitpid(helper_pid, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == helper_pid ? status : -1;
}

bool SpawnOneShot(const std::filesystem::path& helper,
                  std::string_view option,
                  std::string_view message) {
  std::string helper_string = helper.string();
  std::string option_string(option);
  std::string message_string(message.substr(0, kMaximumMessageBytes));
  char* arguments[] = {helper_string.data(), option_string.data(),
                       message_string.data(), nullptr};
  pid_t child = -1;
  const auto child_environment = BuildHelperEnvironment(environ);
  const int spawn_status =
      posix_spawn(&child, helper.c_str(), nullptr, nullptr, arguments,
                   child_environment.data());
  if (spawn_status != 0) {
    return false;
  }
  (void)WaitForHelper(child);
  return true;
}

ConfirmationDialogResult MapConfirmationStatus(int status) {
  if (status < 0 || !WIFEXITED(status)) {
    return ConfirmationDialogResult::kUnavailable;
  }
  switch (WEXITSTATUS(status)) {
    case 0:
      return ConfirmationDialogResult::kConfirmed;
    case 2:
      return ConfirmationDialogResult::kCancelled;
    default:
      return ConfirmationDialogResult::kUnavailable;
  }
}

ConfirmationDialogResult SpawnConfirmation(
    const std::filesystem::path& helper, std::string_view message) {
  std::string helper_string = helper.string();
  std::string option_string("--confirm");
  std::string message_string(message.substr(0, kMaximumMessageBytes));
  char* arguments[] = {helper_string.data(), option_string.data(),
                       message_string.data(), nullptr};
  pid_t child = -1;
  const int spawn_status =
      posix_spawn(&child, helper.c_str(), nullptr, nullptr, arguments, environ);
  if (spawn_status != 0) {
    return ConfirmationDialogResult::kUnavailable;
  }
  return MapConfirmationStatus(WaitForHelper(child));
}

bool SendConfirmationPacket(int socket, bool ready,
                            std::string_view message) {
  if (socket < 0) return false;
  std::string packet(1, ready ? kConfirmationReadyPacket
                              : kConfirmationErrorPacket);
  packet.append(message.substr(0, kMaximumMessageBytes));
  ssize_t sent = 0;
  do {
    sent = send(socket, packet.data(), packet.size(), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent == static_cast<ssize_t>(packet.size());
}

ConfirmationDialogResult SpawnConfirmationWhenReady(
    const std::filesystem::path& helper, std::string_view message,
    void* context, ConfirmationReadyFn ready) {
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
    return ConfirmationDialogResult::kUnavailable;
  }
  // The child receives the socket at fd 3. If socketpair happens to return
  // fd 3, dup2(3, 3) would not clear close-on-exec, so clear it explicitly on
  // the child descriptor before spawning.
  const int child_fd_flags = fcntl(sockets[1], F_GETFD);
  if (child_fd_flags < 0 ||
      fcntl(sockets[1], F_SETFD, child_fd_flags & ~FD_CLOEXEC) != 0) {
    close(sockets[0]);
    close(sockets[1]);
    return ConfirmationDialogResult::kUnavailable;
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(sockets[0]);
    close(sockets[1]);
    return ConfirmationDialogResult::kUnavailable;
  }
  int action_status = 0;
  if (action_status == 0 && sockets[1] != kConfirmationReadyFd) {
    action_status = posix_spawn_file_actions_adddup2(
        &actions, sockets[1], kConfirmationReadyFd);
  }
  if (action_status == 0 && sockets[0] != kConfirmationReadyFd) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  }
  if (action_status == 0 && sockets[1] != kConfirmationReadyFd) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[1]);
  }

  std::string helper_string = helper.string();
  std::string option_string("--confirm-ready");
  std::string message_string(message.substr(0, kMaximumMessageBytes));
  char* arguments[] = {helper_string.data(), option_string.data(),
                       message_string.data(), nullptr};
  pid_t child = -1;
  int spawn_status = action_status;
  if (spawn_status == 0) {
    spawn_status = posix_spawn(&child, helper.c_str(), &actions, nullptr,
                               arguments, environ);
  }
  posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (spawn_status != 0) {
    close(sockets[0]);
    return ConfirmationDialogResult::kUnavailable;
  }

  std::string ready_message;
  Status ready_status =
      ready != nullptr ? ready(context, &ready_message) : Status::Ok();
  if (ready_status.ok() && ready_message.empty()) {
    ready_status = Status::Error(
        StatusCode::kUnavailable, "confirmation readiness returned no message");
  }
  if (!ready_status.ok() && ready_message.empty()) {
    ready_message = "Roblox could not verify this game.";
  }
  (void)SendConfirmationPacket(sockets[0], ready_status.ok(), ready_message);
  close(sockets[0]);

  const ConfirmationDialogResult result =
      MapConfirmationStatus(WaitForHelper(child));
  if (!ready_status.ok() && result == ConfirmationDialogResult::kConfirmed) {
    return ConfirmationDialogResult::kUnavailable;
  }
  return result;
}

std::pair<int, int> SpawnMonitor(const std::filesystem::path& helper) {
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
    return {-1, -1};
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(sockets[0]);
    close(sockets[1]);
    return {-1, -1};
  }
  int action_status = 0;
  if (sockets[0] == STDIN_FILENO) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  }
  if (action_status == 0 && sockets[1] != STDIN_FILENO) {
    action_status =
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
  }
  if (action_status == 0 && sockets[0] != STDIN_FILENO) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  }
  if (action_status == 0 && sockets[1] != STDIN_FILENO) {
    action_status = posix_spawn_file_actions_addclose(&actions, sockets[1]);
  }

  std::string helper_string = helper.string();
  char monitor_option[] = "--monitor";
  char* arguments[] = {helper_string.data(), monitor_option, nullptr};
  pid_t child = -1;
  int spawn_status = action_status;
  if (spawn_status == 0) {
    const auto child_environment = BuildHelperEnvironment(environ);
    spawn_status = posix_spawn(&child, helper.c_str(), &actions, nullptr,
                               arguments, child_environment.data());
  }
  posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (spawn_status != 0) {
    close(sockets[0]);
    return {-1, -1};
  }
  return {sockets[0], static_cast<int>(child)};
}

bool SendPacket(int socket, char kind, std::string_view payload = {}) {
  std::string packet(1, kind);
  packet.append(payload.substr(0, kMaximumMessageBytes));
  const ssize_t sent = send(socket, packet.data(), packet.size(), MSG_NOSIGNAL);
  return sent == static_cast<ssize_t>(packet.size());
}

}  // namespace

bool FailureDialogsEnabled(const Environment& environment) {
  if (Enabled(environment, "MOCKTAIL_DISABLE_FAILURE_DIALOG") ||
      Enabled(environment, "MOCKTAIL_ISOLATED_CANARY") ||
      Enabled(environment, "MOCKTAIL_HEADLESS")) {
    return false;
  }
  return HasNonEmpty(environment, "DISPLAY") ||
         HasNonEmpty(environment, "WAYLAND_DISPLAY");
}

bool ShowFailureDialog(const Environment& environment,
                       std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return false;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return !helper.empty() && SpawnOneShot(helper, "--message", message);
}

bool ShowWarningDialog(const Environment& environment,
                       std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return false;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return !helper.empty() && SpawnOneShot(helper, "--warning", message);
}

ConfirmationDialogResult ShowConfirmationDialog(
    const Environment& environment, std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return ConfirmationDialogResult::kUnavailable;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return helper.empty() ? ConfirmationDialogResult::kUnavailable
                        : SpawnConfirmation(helper, message);
}

ConfirmationDialogResult ShowConfirmationDialog(
    const Environment& environment, std::string_view initial_message,
    void* context, ConfirmationReadyFn ready) {
  if (!FailureDialogsEnabled(environment)) {
    return ConfirmationDialogResult::kUnavailable;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  if (helper.empty()) {
    return ConfirmationDialogResult::kUnavailable;
  }
  if (ready == nullptr) {
    return SpawnConfirmation(helper, initial_message);
  }
  return SpawnConfirmationWhenReady(helper, initial_message, context, ready);
}

FailureDialogMonitor::FailureDialogMonitor(int socket, int helper_pid)
    : socket_(socket), helper_pid_(helper_pid) {}

FailureDialogMonitor::~FailureDialogMonitor() { Finish(kFailurePacket); }

FailureDialogMonitor::FailureDialogMonitor(
    FailureDialogMonitor&& other) noexcept
    : socket_(std::exchange(other.socket_, -1)),
      helper_pid_(std::exchange(other.helper_pid_, -1)) {}

FailureDialogMonitor& FailureDialogMonitor::operator=(
    FailureDialogMonitor&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Finish(kFailurePacket);
  socket_ = std::exchange(other.socket_, -1);
  helper_pid_ = std::exchange(other.helper_pid_, -1);
  return *this;
}

FailureDialogMonitor FailureDialogMonitor::Start(
    const Environment& environment, std::string_view initial_message) {
  if (!FailureDialogsEnabled(environment)) {
    return {};
  }
  const std::filesystem::path helper = DialogHelper(environment);
  if (helper.empty()) {
    return {};
  }
  const auto [socket, helper_pid] = SpawnMonitor(helper);
  FailureDialogMonitor monitor(socket, helper_pid);
  if (monitor.active()) {
    monitor.SetMessage(initial_message);
  }
  return monitor;
}

void FailureDialogMonitor::SetMessage(std::string_view message) {
  if (!active() || message.empty()) {
    return;
  }
  if (!SendPacket(socket_, kMessagePacket, message)) {
    close(socket_);
    socket_ = -1;
    WaitForHelper(helper_pid_);
    helper_pid_ = -1;
  }
}

void FailureDialogMonitor::MarkSuccessful() { Finish(kSuccessPacket); }

void FailureDialogMonitor::Finish(char disposition) {
  if (!active()) {
    return;
  }
  (void)SendPacket(socket_, disposition);
  (void)shutdown(socket_, SHUT_RDWR);
  close(socket_);
  socket_ = -1;
  WaitForHelper(helper_pid_);
  helper_pid_ = -1;
}

}  // namespace runtime
}  // namespace mocktail
