#ifndef MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
#define MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

namespace mocktail {
namespace graphics {

enum class PresentModePolicy {
  kHostDefault,
  kVsync,
  kUnthrottled,
  kFifo,
  kMailbox,
  kImmediate,
};

PresentModePolicy ResolvePresentModePolicy(std::string_view vsync_value,
                                           std::string_view frame_rate_value,
                                           std::string_view present_mode = "auto");

std::optional<VkPresentModeKHR> ExplicitPresentMode(PresentModePolicy policy);
int ResolveSwapInterval(PresentModePolicy policy, int requested);

PresentModePolicy CachedPresentModePolicy();

std::vector<VkPresentModeKHR>
FilterPresentModes(PresentModePolicy policy,
                   const std::vector<VkPresentModeKHR> &host_modes);
const char *PresentModePolicyName(PresentModePolicy policy);
const char *PresentModeKhrName(VkPresentModeKHR mode);

// Creates the swapchain with the mode in `info`. A driver can reject a mode
// that the device did not enable, so a failed create is retried once with the
// mode the game asked for. `create` takes the info and returns a VkResult.
template <typename Create>
VkResult CreateSwapchainWithPresentModeFallback(VkSwapchainCreateInfoKHR &info,
                                                VkPresentModeKHR requested,
                                                Create create) {
  const VkResult result = create(info);
  if (result == VK_SUCCESS || info.presentMode == requested) {
    return result;
  }
  switch (result) {
  case VK_ERROR_OUT_OF_HOST_MEMORY:
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
  case VK_ERROR_DEVICE_LOST:
  case VK_ERROR_SURFACE_LOST_KHR:
  case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
    return result; // Another present mode cannot fix these.
  default:
    break;
  }
  std::fprintf(stderr,
               "  [vulkan] swapchain presentMode %s failed (%d); retrying "
               "with %s\n",
               PresentModeKhrName(info.presentMode), static_cast<int>(result),
               PresentModeKhrName(requested));
  info.presentMode = requested;
  return create(info);
}

// Extra images so acquire does not wait on the compositor-held buffer.
// `fixup` raises the floor to the surface minimum plus one image, so a client
// that asks for a single image still gets a buffer to render into while the
// compositor holds another.
std::uint32_t PreferSwapchainMinImageCount(PresentModePolicy policy,
                                           std::uint32_t requested,
                                           std::uint32_t min_images,
                                           std::uint32_t max_images,
                                           int fixup = 0);

// Payload-scoped swapchain workarounds, published from the Build-ID-scoped
// compatibility profile as MOCKTAIL_SWAPCHAIN_FIXUP. The ladder is monotone:
// every level includes the ones below it, and level 0 is the stock behavior
// this runtime has always had.
enum class SwapchainFixupLevel {
  kNone = 0,           // stock
  kClampExtent = 1,    // + clamp the extent the client asks for to the surface
  kImageFloor = 2,     // + one image above the surface minimum
  kMinimalFallback = 3 // + one last retry with FIFO and a clamped extent
};

// An unrecognized value, including one above SwapchainFixupLevel::kMinimalFallback,
// resolves to stock: a typo must never select the most invasive level.
SwapchainFixupLevel ResolveSwapchainFixupLevel(std::string_view value);

// Reads MOCKTAIL_SWAPCHAIN_FIXUP once, like CachedPresentModePolicy: the
// adapter runs on guest-created threads and re-reading getenv there is racy.
SwapchainFixupLevel CachedSwapchainFixupLevel();

// Clamps a client-requested extent into the surface's supported range. Level 0
// returns the extent untouched.
VkExtent2D ClampSwapchainExtent(VkExtent2D requested, VkExtent2D min_extent,
                                 VkExtent2D max_extent,
                                 SwapchainFixupLevel fixup);

// Creates the swapchain, walking the fixup fallbacks in order: first the
// present-mode retry every policy gets, then, from level 3, one minimal
// swapchain that uses only what the Vulkan specification requires a
// driver to support.
template <typename Create>
VkResult CreateSwapchainWithFixupFallbacks(VkSwapchainCreateInfoKHR &info,
                                           VkPresentModeKHR requested,
                                           SwapchainFixupLevel fixup,
                                           Create create) {
  VkResult result =
      CreateSwapchainWithPresentModeFallback(info, requested, create);
  if (result == VK_SUCCESS ||
      fixup < SwapchainFixupLevel::kMinimalFallback ||
      (info.presentMode == VK_PRESENT_MODE_FIFO_KHR && info.flags == 0)) {
    return result;
  }
  // The earlier fallbacks already failed, so this one asks for the least the
  // specification guarantees of any driver: the mandatory present mode and no
  // optional creation flags.
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  info.flags = 0;
  std::fprintf(stderr,
               "  [vulkan] swapchain creation failed (%d); retrying with a "
               "minimal FIFO swapchain\n",
               static_cast<int>(result));
  result = create(info);
  return result;
}

} // namespace graphics
} // namespace mocktail

#endif // MOCKTAIL_GRAPHICS_PRESENT_MODE_POLICY_H_
