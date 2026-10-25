#include "window/window_pointer_event_pump.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "mocktail/platform/sdl_event_converter.h"
#include "runtime/roblox_input_router.h"
#include "window/window_pointer_capture_owner.h"

namespace mocktail {
namespace window {
namespace {

struct MoveCall {
  float x = 0.0f;
  float y = 0.0f;
  float delta_x = 0.0f;
  float delta_y = 0.0f;
};

struct ButtonCall {
  float x = 0.0f;
  float y = 0.0f;
  bool pressed = false;
  int32_t button = -1;
};

// What the host forwarded as it arrived, and what Roblox was finally told.
// Both matter: the capture tag decides whether the delta is look input.
struct Probe {
  std::vector<bool> relative;
  std::vector<MoveCall> moves;
  std::vector<ButtonCall> buttons;

  void Clear() {
    relative.clear();
    moves.clear();
    buttons.clear();
  }
};

Status OnMove(void* context, float x, float y, float delta_x, float delta_y) {
  static_cast<Probe*>(context)->moves.push_back({x, y, delta_x, delta_y});
  return Status::Ok();
}

Status OnButton(void* context, float x, float y, bool pressed,
                int32_t button) {
  static_cast<Probe*>(context)->buttons.push_back({x, y, pressed, button});
  return Status::Ok();
}

Status OnWheel(void* /*context*/, float /*x*/, float /*y*/,
               float /*delta_y*/) {
  return Status::Ok();
}

// Models the client as this host sees it: it can ask for the centre lock, as in
// first person or shift lock, and a pan is reported for the probe a
// right-button press makes.
struct LockState {
  bool center = false;
};

bool QueryLock(void* context, MouseLock* lock) {
  auto* state = static_cast<LockState*>(context);
  lock->center = state->center;
  lock->reports_current_position = true;
  lock->current_position = lock->probe_current_position;
  return true;
}

// The owner sees frames presented by the client and a clock; both decide how
// long a native lock that just dropped keeps the capture.
std::uint64_t g_presented_frames = 0;
std::uint64_t g_now_ms = 0;

std::uint64_t PresentedFrames() { return g_presented_frames; }
std::uint64_t NowMs() { return g_now_ms; }

class WindowEventPumpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* driver = std::getenv("SDL_VIDEODRIVER");
    had_driver_ = driver != nullptr;
    saved_driver_ = driver != nullptr ? driver : "";
    // The pump and the converter need a window object, not a compositor. X11 or
    // Wayland would make this test need a display for no extra coverage.
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
    window_ = SDL_CreateWindow("Mocktail event pump test", 800, 600,
                               SDL_WINDOW_HIDDEN);
    ASSERT_NE(window_, nullptr) << SDL_GetError();

    g_presented_frames = 0;
    g_now_ms = 0;
    backend_ = std::make_unique<SdlPointerCaptureBackend>(window_);
    owner_ = std::make_unique<WindowPointerCaptureOwner>(
        backend_.get(), &PresentedFrames, &NowMs);
    owner_->SetGameSessionActive(true);
    ASSERT_TRUE(owner_->RegisterQuery(&QueryLock, &lock_));

    runtime::RobloxInputSink sink;
    sink.context = &probe_;
    sink.mouse_move = &OnMove;
    sink.mouse_button = &OnButton;
    sink.mouse_wheel = &OnWheel;
    router_ = std::make_unique<runtime::RobloxInputRouter>(sink);
    ASSERT_TRUE(
        router_->Activate({800, 600, 800, 600, 1.0f}, true).ok());
    pump_ = std::make_unique<WindowPointerEventPump>(owner_.get(), &Forward,
                                                     this);
  }

  void TearDown() override {
    pump_.reset();
    router_.reset();
    owner_.reset();
    backend_.reset();
    if (window_ != nullptr) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    SDL_Quit();
    if (had_driver_) {
      setenv("SDL_VIDEODRIVER", saved_driver_.c_str(), 1);
    } else {
      unsetenv("SDL_VIDEODRIVER");
    }
  }

  // The sole consumer of converted events, exactly as the window loop feeds it.
  static void Forward(void* context, const platform::PlatformEvent& event) {
    auto* test = static_cast<WindowEventPumpTest*>(context);
    if (const auto* motion =
            std::get_if<platform::MouseMotionEvent>(&event.payload)) {
      test->probe_.relative.push_back(motion->relative);
    }
    test->router_->HandleEvent(event);
  }

  void Warp(float x, float y) { SDL_WarpMouseInWindow(window_, x, y); }

  // One turn of the window loop: the capture state is reapplied, then a frame
  // is presented. A capture that just ended has its recentred pointer reported
  // to the client the way the window loop does it.
  void Frame() {
    owner_->Pump(/*text_input_active=*/false);
    if (owner_->TakePointerRecenter()) {
      platform::PlatformEvent event;
      event.timestamp_ns = g_now_ms * 1000000ULL;
      platform::MouseMotionEvent recenter;
      recenter.x = 400.0f;
      recenter.y = 300.0f;
      recenter.buttons =
          static_cast<std::uint32_t>(SDL_GetMouseState(nullptr, nullptr));
      event.payload = recenter;
      router_->HandleEvent(event);
    }
    ++g_presented_frames;
    g_now_ms += 4;
  }

  void PushMotion(float x, float y, float delta_x, float delta_y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = SDL_GetWindowID(window_);
    event.motion.state = SDL_GetMouseState(nullptr, nullptr);
    event.motion.x = x;
    event.motion.y = y;
    event.motion.xrel = delta_x;
    event.motion.yrel = delta_y;
    ASSERT_TRUE(SDL_PushEvent(&event)) << SDL_GetError();
  }

  void PushButton(uint8_t button, bool down, float x, float y) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.windowID = SDL_GetWindowID(window_);
    event.button.button = button;
    event.button.down = down;
    event.button.clicks = 1;
    event.button.x = x;
    event.button.y = y;
    ASSERT_TRUE(SDL_PushEvent(&event)) << SDL_GetError();
  }

  // Drains the SDL queue the way PumpEvents does: convert, hand to the pump,
  // flush the motion it is holding whenever anything else arrives.
  void Drain(bool text_input_active = false) {
    SDL_Event event{};
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST,
                          SDL_EVENT_LAST) > 0) {
      if (event.type != SDL_EVENT_MOUSE_MOTION) {
        pump_->Flush();
      }
      platform::PlatformEvent converted;
      if (platform::ConvertSdlEvent(window_, event, &converted)) {
        pump_->Handle(converted, text_input_active);
      }
    }
    pump_->Flush();
  }

  float PointerX() {
    float x = 0.0f;
    SDL_GetMouseState(&x, nullptr);
    return x;
  }

  float PointerY() {
    float y = 0.0f;
    SDL_GetMouseState(nullptr, &y);
    return y;
  }

  bool had_driver_ = false;
  std::string saved_driver_;
  SDL_Window* window_ = nullptr;
  std::unique_ptr<SdlPointerCaptureBackend> backend_;
  std::unique_ptr<WindowPointerCaptureOwner> owner_;
  std::unique_ptr<runtime::RobloxInputRouter> router_;
  std::unique_ptr<WindowPointerEventPump> pump_;
  Probe probe_;
  LockState lock_;
};

TEST_F(WindowEventPumpTest, CoalescesAFastDragIntoSeparateCaptureTaggedMotion) {
  Warp(400.0f, 300.0f);
  Drain();
  probe_.Clear();

  PushButton(SDL_BUTTON_RIGHT, true, 400.0f, 300.0f);
  Drain();
  ASSERT_TRUE(owner_->captured());
  EXPECT_TRUE(SDL_GetWindowRelativeMouseMode(window_));

  PushMotion(400.0f, 300.0f, 12.0f, 4.0f);
  PushMotion(400.0f, 300.0f, 8.0f, -6.0f);
  PushMotion(400.0f, 300.0f, 5.0f, 1.0f);
  Drain();

  ASSERT_EQ(probe_.moves.size(), 1U);
  ASSERT_EQ(probe_.relative.size(), 1U);
  EXPECT_TRUE(probe_.relative[0]);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_x, 25.0f);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_y, -1.0f);
  // The pointer Roblox draws stays where the drag was captured.
  EXPECT_FLOAT_EQ(probe_.moves[0].x, 400.0f);
  EXPECT_FLOAT_EQ(probe_.moves[0].y, 300.0f);
}

TEST_F(WindowEventPumpTest, ReleaseEndsTheDragWithOneRestoreAtThePressPoint) {
  Warp(400.0f, 300.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, true, 400.0f, 300.0f);
  Drain();
  ASSERT_TRUE(owner_->captured());
  probe_.Clear();

  // The pointer is still moving when the button comes up: the last of the drag
  // and the release arrive in the same batch.
  PushMotion(400.0f, 300.0f, 40.0f, 12.0f);
  PushButton(SDL_BUTTON_RIGHT, false, 400.0f, 300.0f);
  Drain();

  EXPECT_FALSE(owner_->captured());
  EXPECT_FALSE(SDL_GetWindowRelativeMouseMode(window_));
  ASSERT_EQ(probe_.moves.size(), 2U);
  // The drag ahead of the release is still camera input.
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_x, 40.0f);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_y, 12.0f);
  EXPECT_TRUE(probe_.relative[0]);

  // The restore is a separate event, in absolute coordinates, on the press
  // point, and carries no look delta.
  EXPECT_FALSE(probe_.relative[1]);
  EXPECT_FLOAT_EQ(probe_.moves[1].x, 400.0f);
  EXPECT_FLOAT_EQ(probe_.moves[1].y, 300.0f);
  EXPECT_FLOAT_EQ(probe_.moves[1].delta_x, 0.0f);
  EXPECT_FLOAT_EQ(probe_.moves[1].delta_y, 0.0f);

  // The desktop pointer is back where the button went down.
  EXPECT_FLOAT_EQ(PointerX(), 400.0f);
  EXPECT_FLOAT_EQ(PointerY(), 300.0f);
}

TEST_F(WindowEventPumpTest, MotionAfterTheReleaseIsNotLookInput) {
  Warp(400.0f, 300.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, true, 400.0f, 300.0f);
  Drain();
  ASSERT_TRUE(owner_->captured());
  PushMotion(400.0f, 300.0f, 120.0f, 40.0f);
  PushButton(SDL_BUTTON_RIGHT, false, 400.0f, 300.0f);
  Drain();
  ASSERT_FALSE(owner_->captured());

  // Releasing mid-drag leaves the client's own motion behind the restore: a
  // re-synced position and whatever the desktop queued while the pointer was
  // still captured. Their deltas span the drag, so none of them may reach the
  // camera.
  probe_.Clear();
  PushMotion(600.0f, 450.0f, -600.0f, 200.0f);
  PushMotion(700.0f, 505.0f, 10.0f, 5.0f);
  Drain();

  ASSERT_EQ(probe_.moves.size(), 1U);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_x, 0.0f);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_y, 0.0f);
  // The reported position still follows the pointer, so the cursor keeps
  // tracking it and the next press is accurate.
  EXPECT_FLOAT_EQ(probe_.moves[0].x, 700.0f);
  EXPECT_FLOAT_EQ(probe_.moves[0].y, 505.0f);

  // A press starts a drag the host does not capture, and its absolute motion
  // carries genuine deltas again.
  PushButton(SDL_BUTTON_LEFT, true, 700.0f, 505.0f);
  Drain();
  probe_.Clear();
  PushMotion(710.0f, 510.0f, 10.0f, 5.0f);
  Drain();
  ASSERT_EQ(probe_.moves.size(), 1U);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_x, 10.0f);
  EXPECT_FLOAT_EQ(probe_.moves[0].delta_y, 5.0f);
}

TEST_F(WindowEventPumpTest, EveryDragRestoresToItsOwnPressPoint) {
  // A drag's relative motion moves the client's idea of the pointer far past
  // the window edge. Restoring to anything read during or after the drag rather
  // than at the press walks the pointer away from where the user clicked, one
  // drag at a time.
  for (int drag = 0; drag < 40; ++drag) {
    const float press_x = 120.0f + static_cast<float>((drag * 37) % 560);
    const float press_y = 90.0f + static_cast<float>((drag * 53) % 420);
    Warp(press_x, press_y);
    Drain();
    PushButton(SDL_BUTTON_RIGHT, true, press_x, press_y);
    Drain();
    ASSERT_TRUE(owner_->captured()) << "drag " << drag;

    for (int step = 0; step < 6; ++step) {
      PushMotion(press_x, press_y, 90.0f, 30.0f);
    }
    Drain();
    PushButton(SDL_BUTTON_RIGHT, false, press_x, press_y);
    Drain();
    ASSERT_FALSE(owner_->captured()) << "drag " << drag;

    EXPECT_FLOAT_EQ(PointerX(), press_x) << "drag " << drag;
    EXPECT_FLOAT_EQ(PointerY(), press_y) << "drag " << drag;
    probe_.Clear();
  }
}

TEST_F(WindowEventPumpTest, ReleaseWithTextFocusNeverCaptures) {
  Warp(300.0f, 200.0f);
  Drain();
  probe_.Clear();

  PushButton(SDL_BUTTON_RIGHT, true, 300.0f, 200.0f);
  Drain(/*text_input_active=*/true);
  EXPECT_FALSE(owner_->captured());
  EXPECT_FALSE(SDL_GetWindowRelativeMouseMode(window_));

  probe_.Clear();
  PushMotion(310.0f, 205.0f, 10.0f, 5.0f);
  PushButton(SDL_BUTTON_RIGHT, false, 310.0f, 205.0f);
  Drain(/*text_input_active=*/true);
  EXPECT_FALSE(owner_->captured());
  ASSERT_EQ(probe_.moves.size(), 1U);
  EXPECT_FALSE(probe_.relative[0]);
  // A press that never captured the pointer leaves the cursor where the user
  // left it.
  EXPECT_FLOAT_EQ(PointerX(), 300.0f);
  EXPECT_FLOAT_EQ(PointerY(), 200.0f);
}

TEST_F(WindowEventPumpTest, FirstPersonDragThenThirdPersonDragRestoresItsOwnPoint) {
  // First person locks the pointer to the middle of the window. A drag there
  // turns the camera and leaves the pointer where the lock put it.
  lock_.center = true;
  Frame();
  ASSERT_TRUE(owner_->captured());
  EXPECT_FLOAT_EQ(PointerX(), 400.0f);
  EXPECT_FLOAT_EQ(PointerY(), 300.0f);

  PushButton(SDL_BUTTON_RIGHT, true, 400.0f, 300.0f);
  Drain();
  PushMotion(400.0f, 300.0f, 30.0f, 10.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, false, 400.0f, 300.0f);
  Drain();
  EXPECT_TRUE(owner_->captured());

  // Zoomed back out: the client drops the lock, which gives the capture a few
  // frames' grace, and then the pointer comes back to the middle.
  lock_.center = false;
  for (int frame = 0; frame < 20; ++frame) {
    Frame();
  }
  EXPECT_FALSE(owner_->captured());
  EXPECT_FLOAT_EQ(PointerX(), 400.0f);
  EXPECT_FLOAT_EQ(PointerY(), 300.0f);

  // A third-person drag somewhere else still comes back to its own press.
  Warp(620.0f, 180.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, true, 620.0f, 180.0f);
  Drain();
  ASSERT_TRUE(owner_->captured());
  PushMotion(620.0f, 180.0f, 30.0f, 5.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, false, 620.0f, 180.0f);
  Drain();
  EXPECT_FALSE(owner_->captured());
  EXPECT_FLOAT_EQ(PointerX(), 620.0f);
  EXPECT_FLOAT_EQ(PointerY(), 180.0f);
}

TEST_F(WindowEventPumpTest, DragStraightAfterFirstPersonRestoresItsOwnPoint) {
  // Dragging moments after the client drops its centre lock used to inherit
  // that lock's restore point, so letting go sent the pointer back to the
  // middle of the window instead of where the button went down.
  lock_.center = true;
  Frame();
  ASSERT_TRUE(owner_->captured());
  PushButton(SDL_BUTTON_RIGHT, true, 400.0f, 300.0f);
  Drain();
  PushMotion(400.0f, 300.0f, 30.0f, 10.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, false, 400.0f, 300.0f);
  Drain();

  lock_.center = false;
  Frame();  // the grace still holds the capture for this frame
  Warp(620.0f, 180.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, true, 620.0f, 180.0f);
  Drain();
  PushMotion(620.0f, 180.0f, 40.0f, 10.0f);
  Drain();
  PushButton(SDL_BUTTON_RIGHT, false, 620.0f, 180.0f);
  Drain();

  for (int frame = 0; frame < 20; ++frame) {
    Frame();
  }
  EXPECT_FALSE(owner_->captured());
  EXPECT_FLOAT_EQ(PointerX(), 620.0f);
  EXPECT_FLOAT_EQ(PointerY(), 180.0f);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
