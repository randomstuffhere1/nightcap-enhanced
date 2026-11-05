#ifndef MOCKTAIL_RUNTIME_FAILURE_DIALOG_H_
#define MOCKTAIL_RUNTIME_FAILURE_DIALOG_H_

#include <string>
#include <filesystem>
#include <string_view>

#include "mocktail/status.h"
#include "runtime/environment.h"

namespace mocktail {
namespace runtime {

// Canaries and headless sessions remain non-interactive.
bool FailureDialogsEnabled(const Environment& environment);

bool ShowFailureDialog(const Environment& environment,
                       std::string_view message);
bool ShowWarningDialog(const Environment& environment,
                       std::string_view message);

enum class ConfirmationDialogResult {
  kUnavailable,
  kCancelled,
  kConfirmed,
};

// Opens a modal Yes/No prompt. A closed window and the No button both cancel;
// the caller must never interpret an unavailable prompt as approval.
ConfirmationDialogResult ShowConfirmationDialog(
    const Environment& environment, std::string_view message);

// Called after the confirmation helper has been spawned. The returned message
// is shown in the already-visible dialog. A non-OK status leaves Yes disabled.
using ConfirmationReadyFn = Status (*)(void* context,
                                        std::string* ready_message);

ConfirmationDialogResult ShowConfirmationDialog(
    const Environment& environment, std::string_view initial_message,
    void* context, ConfirmationReadyFn ready);

// The helper starts before guest threads, so it can report errors and fatal
// exits even when the main process can no longer show a dialog.
class FailureDialogMonitor final {
 public:
  FailureDialogMonitor() = default;
  ~FailureDialogMonitor();

  FailureDialogMonitor(const FailureDialogMonitor&) = delete;
  FailureDialogMonitor& operator=(const FailureDialogMonitor&) = delete;
  FailureDialogMonitor(FailureDialogMonitor&& other) noexcept;
  FailureDialogMonitor& operator=(FailureDialogMonitor&& other) noexcept;

  static FailureDialogMonitor Start(const Environment& environment,
                                    std::string_view initial_message);

  bool active() const { return socket_ >= 0; }
  void SetMessage(std::string_view message);
  // Adds a button to the failure dialog that opens this folder.
  void SetLogFolder(const std::filesystem::path& folder);
  void MarkSuccessful();

 private:
  FailureDialogMonitor(int socket, int helper_pid);
  void Send(char kind, std::string_view payload);
  void Finish(char disposition);

  int socket_ = -1;
  int helper_pid_ = -1;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_FAILURE_DIALOG_H_
