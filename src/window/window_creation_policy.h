#ifndef MOCKTAIL_WINDOW_WINDOW_CREATION_POLICY_H_
#define MOCKTAIL_WINDOW_WINDOW_CREATION_POLICY_H_

#include <SDL3/SDL_video.h>

#include <string_view>

namespace mocktail {
namespace window {

SDL_WindowFlags ApplyHighPixelDensityWindowFlag(SDL_WindowFlags flags,
                                                bool high_dpi);

// Native Wayland surfaces must receive their initial configure before Vulkan
// can reliably query the surface extent. X11/XWayland windows do not have
// that requirement, and waiting for an asynchronous window-manager state
// change (for example, restored maximization) can time out even though the
// X11 window is already usable.
bool DirectVulkanWindowRequiresInitialSync(std::string_view video_driver);

// Exclusive fullscreen needs a backend that can switch display modes. Wayland
// has no equivalent, so a request there stays borderless.
bool SupportsExclusiveFullscreen(std::string_view video_driver);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_CREATION_POLICY_H_
