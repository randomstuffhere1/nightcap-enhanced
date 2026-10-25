#include "window/window_pointer_event_pump.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <cstdio>
#include <utility>

#include "window/window_pointer_capture_owner.h"

namespace mocktail {
namespace window {

WindowPointerEventPump::WindowPointerEventPump(
    WindowPointerCaptureOwner* owner, Sink sink, void* sink_context)
    : owner_(owner), sink_(sink), sink_context_(sink_context) {}

void WindowPointerEventPump::Flush() {
  if (!has_pending_motion_) {
    return;
  }
  platform::PlatformEvent motion = std::move(pending_motion_);
  pending_motion_ = {};
  has_pending_motion_ = false;
  if (sink_ != nullptr) {
    sink_(sink_context_, motion);
  }
}

void WindowPointerEventPump::Emit(const platform::PlatformEvent& event) {
  if (sink_ != nullptr) {
    sink_(sink_context_, event);
  }
}

void WindowPointerEventPump::Handle(const platform::PlatformEvent& event,
                                    bool text_input_active) {
  const auto* motion = std::get_if<platform::MouseMotionEvent>(&event.payload);
  if (motion == nullptr) {
    // The client sees motion that preceded this event before it sees the event
    // itself, which is what SDL's own order means.
    Flush();
    Emit(event);
  } else if (!has_pending_motion_) {
    pending_motion_ = event;
    has_pending_motion_ = true;
  } else {
    auto* current =
        std::get_if<platform::MouseMotionEvent>(&pending_motion_.payload);
    if (current != nullptr && current->relative == motion->relative) {
      current->delta_x += motion->delta_x;
      current->delta_y += motion->delta_y;
      current->x = motion->x;
      current->y = motion->y;
      current->buttons = motion->buttons;
      pending_motion_.timestamp_ns = event.timestamp_ns;
    } else {
      Flush();
      pending_motion_ = event;
      has_pending_motion_ = true;
    }
  }

  if (owner_ == nullptr) {
    return;
  }
  // Shift can enable the client's own persistent mouse lock while a transient
  // right-button drag is active, so the native state is refreshed after the key
  // is dispatched and the following release preserves that lock.
  if (const auto* key = std::get_if<platform::KeyEvent>(&event.payload)) {
    if (key->pressed && !key->repeat &&
        (key->scancode == SDL_SCANCODE_LSHIFT ||
         key->scancode == SDL_SCANCODE_RSHIFT) &&
        !owner_->OnShiftKeyPressed(text_input_active)) {
      std::fprintf(stderr,
                   "  [input] SDL shift-lock pointer capture update failed\n");
    }
  }
  if (const auto* button =
          std::get_if<platform::MouseButtonEvent>(&event.payload)) {
    if (button->button == SDL_BUTTON_RIGHT &&
        !owner_->OnRightButton(button->pressed, text_input_active)) {
      std::fprintf(stderr,
                   "  [input] SDL right-button pointer capture update failed\n");
    }
  }
}

}  // namespace window
}  // namespace mocktail
