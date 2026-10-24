#include "runtime/texture_memory_policy.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uint64_t kMebibyte = 1024U * 1024U;

TEST(TextureMemoryPolicyTest, LeavesTheGuestDefaultOnSmallHosts) {
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(0), 0U);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(1024U * kMebibyte), 0U);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(2047U * kMebibyte), 0U);
}

TEST(TextureMemoryPolicyTest, SizesABudgetForFourGigabyteHosts) {
  // A "4 GB" laptop reports less than 4096 MiB in MemTotal.
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(3800U * kMebibyte),
            475U * kMebibyte);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(2048U * kMebibyte),
            256U * kMebibyte);
}

TEST(TextureMemoryPolicyTest, SizesAClampedShareOfHostMemory) {
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(4096U * kMebibyte),
            512U * kMebibyte);
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(8192U * kMebibyte),
            1024U * kMebibyte);
  // An eighth of a very large host stays inside the PC-class ceiling.
  EXPECT_EQ(CalculateTextureMemoryBudgetBytes(65536U * kMebibyte),
            1536U * kMebibyte);
}

TEST(TextureMemoryPolicyTest, AnExplicitBudgetOverridesTheAutomaticOne) {
  bool valid = false;
  // 0 means "decide from host memory", so it must reproduce the automatic
  // figure exactly rather than becoming a zero budget.
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(8192U * kMebibyte, 0, &valid),
            1024U * kMebibyte);
  EXPECT_TRUE(valid);

  // Above the automatic ceiling, on purpose: a user asking for more than the
  // eighth-of-host rule allows should get it, not the clamped value back.
  // 2000 MiB is under the FInt's limit; see RejectsABudgetTheClientCannotCarry
  // for why the setting cannot go much higher.
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(8192U * kMebibyte, 2000, &valid),
            2000U * kMebibyte);
  EXPECT_TRUE(valid);
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(4096U * kMebibyte, 1800, &valid),
            1800U * kMebibyte);
  EXPECT_TRUE(valid);

  // Below the automatic floor too, which is the point of asking.
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(65536U * kMebibyte, 512, &valid),
            512U * kMebibyte);
  EXPECT_TRUE(valid);
}

TEST(TextureMemoryPolicyTest, RejectsABudgetTheClientCannotCarry) {
  bool valid = true;
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(8192U * kMebibyte, 70000, &valid),
            0U);
  EXPECT_FALSE(valid);
  // The client reads the budget through a signed 32-bit FInt, so the ceiling
  // is INT32_MAX bytes: 2047 MiB. A budget past that is refused rather than
  // truncated into something that looks honoured. This is why the setting
  // cannot usefully exceed the automatic 1536 MiB ceiling by much.
  valid = true;
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(8192U * kMebibyte, 2048, &valid),
            0U);
  EXPECT_FALSE(valid);
  valid = true;
  EXPECT_EQ(
      ResolveTextureMemoryBudgetBytes(8192U * kMebibyte, 2047, &valid),
      2047U * kMebibyte);
  EXPECT_TRUE(valid);
  // The automatic path is unaffected by the override ceiling.
  valid = false;
  EXPECT_EQ(ResolveTextureMemoryBudgetBytes(8192U * kMebibyte, 0, &valid),
            1024U * kMebibyte);
  EXPECT_TRUE(valid);
}

TEST(TextureMemoryPolicyTest, PublishesTheBudgetWithoutDiscardingOverrides) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(
      1024U * kMebibyte,
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error))
      << error;
  const nlohmann::json parsed = nlohmann::json::parse(merged);
  EXPECT_EQ(parsed.at("FIntRenderForceVideoMemorySize"),
            std::to_string(1024U * kMebibyte));
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanShaderMTDenyPattern"), "4318:.*");
}

TEST(TextureMemoryPolicyTest, KeepsAnExplicitUserOverride) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(
      1024U * kMebibyte, R"({"FIntRenderForceVideoMemorySize":"64"})", &merged,
      &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged).at("FIntRenderForceVideoMemorySize"),
            "64");
}

TEST(TextureMemoryPolicyTest, ZeroBudgetPublishesNothing) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(0, "{}", &merged,
                                                        &error))
      << error;
  EXPECT_FALSE(
      nlohmann::json::parse(merged).contains("FIntRenderForceVideoMemorySize"));
}

TEST(TextureMemoryPolicyTest, ClampsToTheFastVariableRange) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeTextureMemoryClientSettingsOverrides(
      8ULL * 1024U * kMebibyte, "{}", &merged, &error))
      << error;
  EXPECT_EQ(nlohmann::json::parse(merged).at("FIntRenderForceVideoMemorySize"),
            std::to_string(std::numeric_limits<std::int32_t>::max()));
}

TEST(TextureMemoryPolicyTest, RejectsMalformedInput) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeTextureMemoryClientSettingsOverrides(kMebibyte, "[]",
                                                         &merged, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(MergeTextureMemoryClientSettingsOverrides(kMebibyte, "{}",
                                                         nullptr, &error));
}

TEST(TextureMemoryPolicyTest, ReadsHostMemory) {
  // /proc/meminfo is always present on the supported hosts.
  EXPECT_GT(DetectHostMemoryBytes(), 0U);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
