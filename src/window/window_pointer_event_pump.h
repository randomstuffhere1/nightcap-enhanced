#ifndef MOCKTAIL_WINDOW_WINDOW_POINTER_EVENT_PUMP_H_
#define MOCKTAIL_WINDOW_WINDOW_POINTER_EVENT_PUMP_H_

#include "mocktail/platform/platform_runtime.h"

namespace mocktail {
namespace window {

class WindowPointerCaptureOwner;

// Routes the converted SDL events of one pump batch to the pointer capture
// owner and to the platform event observer, in the order SDL delivered them.
//
// Consecutive motion events are coalesced into one, so a fast drag costs the
// client one event per batch instead of one per mouse sample. Coalescing never
// crosses a capture tag change: the absolute motion that puts the pointer back
// at the end of a drag spans the whole drag, and folding it into the relative
// motion before or after it would hand that distance on as look input.
//
// A button or key event is forwarded before the capture owner reacts to it, so
// the client sees the release that ends a drag before the pointer is restored.
class WindowPointerEventPump final {
 public:
  using Sink = void (*)(void* context, const platform::PlatformEvent& event);

  WindowPointerEventPump(WindowPointerCaptureOwner* owner, Sink sink,
                         void* sink_context);

  // Feeds one converted event. `text_input_active` reports host text focus,
  // which suppresses the fallback right-button camera capture.
  void Handle(const platform::PlatformEvent& event, bool text_input_active);
  // Sends whatever motion is waiting. The pump batch ends with one of these.
  void Flush();

  bool has_pending_motion() const { return has_pending_motion_; }

 private:
  void Emit(const platform::PlatformEvent& event);

  WindowPointerCaptureOwner* owner_ = nullptr;
  Sink sink_ = nullptr;
  void* sink_context_ = nullptr;
  platform::PlatformEvent pending_motion_{};
  bool has_pending_motion_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_POINTER_EVENT_PUMP_H_
