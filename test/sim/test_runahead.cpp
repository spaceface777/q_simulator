#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "../../src/sim/runahead.h"

namespace {

	TEST(ExactRunahead, ChildOutputBoundaryItselfIsNotQuiet) {
		const std::array<bool, 4>     granted = {true, true, true, true};
		const std::array<uint32_t, 4> delta   = {1000, 1000, 1000, 1000};
		EXPECT_EQ(sim::runahead::safe_prefix(5000, 8000, granted, 4, delta), 2u);
	}

	TEST(ExactRunahead, SkippedTicksDoNotAdvanceAChildCertificate) {
		const std::array<bool, 6>     granted = {false, true, false, true, true, true};
		const std::array<uint32_t, 6> delta   = {0, 999, 0, 1000, 1000, 1000};
		EXPECT_EQ(sim::runahead::safe_prefix(100, 2099, granted, 6, delta), 3u);
	}

	TEST(ExactRunahead, ExactClockDeltasCanMoveTheBoundaryAcrossAGlobalTick) {
		const std::array<bool, 4>     granted = {true, true, true, true};
		const std::array<uint32_t, 4> delta   = {999, 999, 1000, 1000};
		EXPECT_EQ(sim::runahead::safe_prefix(0, 2000, granted, 4, delta), 2u);
	}

	TEST(ExactRunahead, PastOrImmediateCertificatesForceStrictFallback) {
		const std::array<bool, 1>     granted = {true};
		const std::array<uint32_t, 1> delta   = {1000};
		EXPECT_EQ(sim::runahead::safe_prefix(10, 10, granted, 1, delta), 0u);
		EXPECT_EQ(sim::runahead::safe_prefix(10, 9, granted, 1, delta), 0u);
	}

	TEST(ExactRunahead, NeverCertificateLeavesTheHardCapInControl) {
		const std::array<bool, 3>     granted = {true, true, true};
		const std::array<uint32_t, 3> delta   = {1000, 1000, 1000};
		EXPECT_EQ(sim::runahead::safe_prefix(0, std::numeric_limits<uint64_t>::max(), granted, 3, delta), 3u);
	}

	TEST(ExactRunahead, SimultaneousChildWakeupsSelectTheirSharedBoundary) {
		const std::array<bool, 5>     granted = {true, true, true, true, true};
		const std::array<uint32_t, 5> delta   = {1000, 1000, 1000, 1000, 1000};
		const uint32_t                first   = sim::runahead::safe_prefix(100, 4100, granted, 5, delta);
		const uint32_t                second  = sim::runahead::safe_prefix(900, 4900, granted, 5, delta);
		EXPECT_EQ(std::min(first, second), 3u);
	}

} // namespace
