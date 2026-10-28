// Modified by vii from komaruworld/mocktail. See README "About this fork".
#include "mocktail/graphics/present_mode_policy.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace graphics {
namespace {

TEST(PresentModePolicyTest, DerivesDisplayAndUnlimitedDefaults) {
  // The unmanaged default keeps Roblox's frame cap but still selects the
  // lowest-latency synchronized present mode instead of the engine's FIFO.
  EXPECT_EQ(ResolvePresentModePolicy("auto", "-1"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", ""), PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "display"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "60"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("auto", "unlimited"),
            PresentModePolicy::kUnthrottled);
  EXPECT_EQ(ResolvePresentModePolicy("on", "unlimited"),
            PresentModePolicy::kVsync);
  EXPECT_EQ(ResolvePresentModePolicy("off", "display"),
            PresentModePolicy::kUnthrottled);
}

TEST(PresentModePolicyTest, FiltersWithoutFabricatingHostModes) {
  const std::vector<VkPresentModeKHR> host = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                              VK_PRESENT_MODE_FIFO_KHR};
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kUnthrottled, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_IMMEDIATE_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kUnthrottled,
                               {VK_PRESENT_MODE_FIFO_KHR}),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
}

TEST(PresentModePolicyTest, ExplicitModesOverrideVsyncAndUseSupportedFallbacks) {
  const std::vector<VkPresentModeKHR> host = {
      VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
      VK_PRESENT_MODE_MAILBOX_KHR};
  for (const char* mode : {"fifo", "mailbox", "immediate"}) {
    SCOPED_TRACE(mode);
    const auto policy = ResolvePresentModePolicy("on", "unlimited", mode);
    ASSERT_TRUE(ExplicitPresentMode(policy).has_value());
    EXPECT_EQ(policy, ResolvePresentModePolicy("off", "display", mode));
    EXPECT_EQ(FilterPresentModes(policy, host),
              (std::vector<VkPresentModeKHR>{*ExplicitPresentMode(policy)}));
    EXPECT_EQ(FilterPresentModes(policy, {VK_PRESENT_MODE_FIFO_KHR}),
              (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}));
    EXPECT_TRUE(FilterPresentModes(policy, {}).empty());
    EXPECT_EQ(ResolveSwapInterval(policy, 0),
              policy == PresentModePolicy::kImmediate ? 0 : 1);
  }
  EXPECT_EQ(ResolveSwapInterval(PresentModePolicy::kHostDefault, 2), 2);
  EXPECT_EQ(ResolveSwapInterval(PresentModePolicy::kUnthrottled, 1), 0);
  EXPECT_EQ(ResolveSwapInterval(PresentModePolicy::kVsync, 0), 1);
}

TEST(PresentModePolicyTest, PrefersMailboxForVsyncWhenHostExposesIt) {
  const std::vector<VkPresentModeKHR> host = {VK_PRESENT_MODE_IMMEDIATE_KHR,
                                              VK_PRESENT_MODE_MAILBOX_KHR,
                                              VK_PRESENT_MODE_FIFO_KHR};
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync, host),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_MAILBOX_KHR}));
}

TEST(PresentModePolicyTest, PrefersLatestReadyThenRelaxedOverFifo) {
  EXPECT_EQ(FilterPresentModes(
                PresentModePolicy::kVsync,
                {VK_PRESENT_MODE_FIFO_KHR,
                 VK_PRESENT_MODE_FIFO_LATEST_READY_KHR,
                 VK_PRESENT_MODE_MAILBOX_KHR}),
            (std::vector<VkPresentModeKHR>{
                VK_PRESENT_MODE_FIFO_LATEST_READY_KHR}));
  EXPECT_EQ(FilterPresentModes(PresentModePolicy::kVsync,
                               {VK_PRESENT_MODE_FIFO_KHR,
                                VK_PRESENT_MODE_FIFO_RELAXED_KHR}),
            (std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_RELAXED_KHR}));
}

TEST(PresentModePolicyTest, RequestsExtraSwapchainImagesWhenUnthrottled) {
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kMailbox, 2, 2, 8),
            3U);
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kMailbox, 2, 2, 2),
            2U);
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kFifo, 2, 2, 8), 2U);
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kVsync, 2, 2, 8),
            4U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 2, 2, 8),
      5U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 1, 2, 2),
      2U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kUnthrottled, 2, 2, 0),
      5U);
}

TEST(PresentModePolicyTest, RetriesWithTheGamesModeWhenTheDriverRejectsOurs) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
  std::vector<VkPresentModeKHR> attempts;
  const VkResult result = CreateSwapchainWithPresentModeFallback(
      info, VK_PRESENT_MODE_FIFO_KHR,
      [&](const VkSwapchainCreateInfoKHR &attempt) {
        attempts.push_back(attempt.presentMode);
        return attempt.presentMode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR
                   ? VK_ERROR_INITIALIZATION_FAILED
                   : VK_SUCCESS;
      });
  EXPECT_EQ(result, VK_SUCCESS);
  EXPECT_EQ(attempts, (std::vector<VkPresentModeKHR>{
                          VK_PRESENT_MODE_FIFO_LATEST_READY_KHR,
                          VK_PRESENT_MODE_FIFO_KHR}));
  EXPECT_EQ(info.presentMode, VK_PRESENT_MODE_FIFO_KHR);
}

TEST(PresentModePolicyTest, DoesNotRetryWhenThereIsNothingToFallBackTo) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  int calls = 0;
  EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                info, VK_PRESENT_MODE_FIFO_KHR,
                [&](const VkSwapchainCreateInfoKHR &) {
                  ++calls;
                  return VK_SUCCESS;
                }),
            VK_SUCCESS);
  EXPECT_EQ(calls, 1);

  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  calls = 0;
  EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                info, VK_PRESENT_MODE_FIFO_KHR,
                [&](const VkSwapchainCreateInfoKHR &) {
                  ++calls;
                  return VK_ERROR_SURFACE_LOST_KHR;
                }),
            VK_ERROR_SURFACE_LOST_KHR);
  EXPECT_EQ(calls, 1);
}

TEST(PresentModePolicyTest, DoesNotRetryAfterErrorsAnotherModeCannotFix) {
  for (const VkResult error :
       {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
        VK_ERROR_DEVICE_LOST, VK_ERROR_SURFACE_LOST_KHR,
        VK_ERROR_NATIVE_WINDOW_IN_USE_KHR}) {
    VkSwapchainCreateInfoKHR info{};
    info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
    int calls = 0;
    EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                  info, VK_PRESENT_MODE_FIFO_KHR,
                  [&](const VkSwapchainCreateInfoKHR &) {
                    ++calls;
                    return error;
                  }),
              error);
    EXPECT_EQ(calls, 1) << "error " << static_cast<int>(error);
  }
}

TEST(PresentModePolicyTest, ReturnsTheRetrysResultWhenItAlsoFails) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
  int calls = 0;
  EXPECT_EQ(CreateSwapchainWithPresentModeFallback(
                info, VK_PRESENT_MODE_FIFO_KHR,
                [&](const VkSwapchainCreateInfoKHR &) {
                  return ++calls == 1 ? VK_ERROR_INITIALIZATION_FAILED
                                      : VK_ERROR_OUT_OF_HOST_MEMORY;
                }),
            VK_ERROR_OUT_OF_HOST_MEMORY);
  EXPECT_EQ(calls, 2);
}

TEST(PresentModePolicyTest, ResolvesOnlyImplementedSwapchainFixupLevels) {
  EXPECT_EQ(ResolveSwapchainFixupLevel("0"), SwapchainFixupLevel::kNone);
  EXPECT_EQ(ResolveSwapchainFixupLevel("1"),
            SwapchainFixupLevel::kClampExtent);
  EXPECT_EQ(ResolveSwapchainFixupLevel("2"), SwapchainFixupLevel::kImageFloor);
  EXPECT_EQ(ResolveSwapchainFixupLevel("3"),
            SwapchainFixupLevel::kMinimalFallback);
  // Anything this runtime does not implement, including a level above the
  // highest one, is stock: a typo must never select the most invasive fixup.
  EXPECT_EQ(ResolveSwapchainFixupLevel(""), SwapchainFixupLevel::kNone);
  EXPECT_EQ(ResolveSwapchainFixupLevel("4"), SwapchainFixupLevel::kNone);
  EXPECT_EQ(ResolveSwapchainFixupLevel("-1"), SwapchainFixupLevel::kNone);
  EXPECT_EQ(ResolveSwapchainFixupLevel("on"), SwapchainFixupLevel::kNone);
  EXPECT_EQ(ResolveSwapchainFixupLevel("maybe"), SwapchainFixupLevel::kNone);
}

TEST(PresentModePolicyTest, ClampsTheExtentOnlyFromTheFirstLevelOn) {
  const VkExtent2D requested{3840, 2160};
  const VkExtent2D minimum{64, 64};
  const VkExtent2D maximum{2560, 1440};
  const auto expect_extent = [](VkExtent2D extent, std::uint32_t width,
                                std::uint32_t height) {
    EXPECT_EQ(extent.width, width);
    EXPECT_EQ(extent.height, height);
  };
  expect_extent(ClampSwapchainExtent(requested, minimum, maximum,
                                     SwapchainFixupLevel::kNone),
                requested.width, requested.height);
  expect_extent(ClampSwapchainExtent(requested, minimum, maximum,
                                     SwapchainFixupLevel::kClampExtent),
                2560, 1440);
  expect_extent(ClampSwapchainExtent({16, 16}, minimum, maximum,
                                     SwapchainFixupLevel::kImageFloor),
                minimum.width, minimum.height);
  // A surface that reports no maximum is left alone rather than clamped to 0.
  expect_extent(ClampSwapchainExtent(requested, minimum, {0, 0},
                                     SwapchainFixupLevel::kMinimalFallback),
                requested.width, requested.height);
}

TEST(PresentModePolicyTest, RaisesTheImageFloorOnlyFromTheSecondLevelOn) {
  // A stock host-default policy is untouched: the fixup is not the policy's
  // business.
  EXPECT_EQ(PreferSwapchainMinImageCount(PresentModePolicy::kHostDefault, 1, 2, 8),
            2U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kHostDefault, 1, 2, 8, 2),
      3U);
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kHostDefault, 1, 2, 3, 3),
      3U);
  // Level 1 clamps the extent only and leaves the image count to the policy.
  EXPECT_EQ(
      PreferSwapchainMinImageCount(PresentModePolicy::kHostDefault, 1, 2, 8, 1),
      2U);
}

TEST(PresentModePolicyTest, RetriesWithAMinimalSwapchainOnlyFromTheLastLevel) {
  VkSwapchainCreateInfoKHR info{};
  info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  info.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
  std::vector<VkResult> attempts;
  const VkResult result = CreateSwapchainWithFixupFallbacks(
      info, VK_PRESENT_MODE_FIFO_KHR, SwapchainFixupLevel::kImageFloor,
      [&](const VkSwapchainCreateInfoKHR &) {
        attempts.push_back(VK_ERROR_INITIALIZATION_FAILED);
        return VK_ERROR_INITIALIZATION_FAILED;
      });
  EXPECT_EQ(result, VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(attempts.size(), 2U);
  EXPECT_EQ(info.flags, VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR);

  VkSwapchainCreateInfoKHR minimal{};
  minimal.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  minimal.flags = VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR;
  std::vector<std::uint32_t> flag_attempts;
  const VkResult minimal_result = CreateSwapchainWithFixupFallbacks(
      minimal, VK_PRESENT_MODE_FIFO_KHR,
      SwapchainFixupLevel::kMinimalFallback,
      [&](const VkSwapchainCreateInfoKHR &attempt) {
        flag_attempts.push_back(attempt.flags);
        return VK_ERROR_INITIALIZATION_FAILED;
      });
  EXPECT_EQ(minimal_result, VK_ERROR_INITIALIZATION_FAILED);
  // The policy retry still comes first; only then does the minimal swapchain.
  EXPECT_EQ(flag_attempts,
            (std::vector<std::uint32_t>{VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR,
                                        VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR,
                                        0U}));
  EXPECT_EQ(minimal.presentMode, VK_PRESENT_MODE_FIFO_KHR);
}

} // namespace
} // namespace graphics
} // namespace mocktail
