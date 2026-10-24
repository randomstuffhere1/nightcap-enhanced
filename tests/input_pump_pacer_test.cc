#include "window/input_pump_pacer.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace window {
namespace {

TEST(InputPumpPacerTest, FirstTickAndMissedDeadlineDoNotSleep) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(2000000), kInterval - 1000000);
}

TEST(InputPumpPacerTest, OnTimeRemainderSleepsOnlyUntilNextSlot) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(2000000), kInterval - 1000000);
  EXPECT_EQ(pacer.DelayBeforeNextPump(6500000), kInterval - 1500000);
}

TEST(InputPumpPacerTest, ExactDeadlineDoesNotInsertAnotherFrameDelay) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(5000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(5000000), kInterval);
}

TEST(InputPumpPacerTest, MissedDeadlineRebasesWithoutSleeping) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(12000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(13000000), kInterval - 1000000);
}

TEST(InputPumpPacerTest, ConsecutiveLateTicksDoNotStackSleep) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(12000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(20000000), 0U);
}

TEST(InputPumpPacerTest, ResetStartsAFreshCadence) {
  constexpr uint64_t kInterval = 4000000;
  InputPumpPacer pacer(kInterval);

  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  pacer.Reset();
  EXPECT_EQ(pacer.DelayBeforeNextPump(100000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(101000000), kInterval - 1000000);
}

TEST(InputPumpPacerTest, SetIntervalRestartsAtTheConfiguredRate) {
  InputPumpPacer pacer;
  EXPECT_EQ(pacer.interval_ns(), kProductionInputPumpIntervalNs);

  pacer.SetIntervalNs(1000000000ULL / kMaximumInputPumpHz);
  EXPECT_EQ(pacer.interval_ns(), 2777777U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(1000000), 0U);
  EXPECT_EQ(pacer.DelayBeforeNextPump(2000000), 1777777U);

  pacer.SetIntervalNs(1000000000ULL / kMinimumInputPumpHz);
  EXPECT_EQ(pacer.interval_ns(), 16666666U);
  // The setter also drops the old deadline, so the first tick after a change
  // does not sleep off a deadline from the previous rate.
  EXPECT_EQ(pacer.DelayBeforeNextPump(2000000), 0U);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
