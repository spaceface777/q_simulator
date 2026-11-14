#include "../../src/firmware/crash.h"
#include "../../src/firmware/inprocess_backend.h"

#include <gtest/gtest.h>

namespace {
	int bind(uint32_t, uint8_t, const qsim_env_entry_t*, size_t) {
		return 0;
	}

	int healthy(uint64_t, uint64_t end, const quercus_mailbox_t*, quercus_mailbox_t* output, uint64_t*) {
		const uint8_t data = static_cast<uint8_t>(end);
		return quercus_mailbox_append(output, &data, sizeof(data));
	}

	__attribute__((noinline)) int fault(uint64_t, uint64_t, const quercus_mailbox_t*, quercus_mailbox_t* output,
										uint64_t*) {
		const uint8_t partial = 123;
		quercus_mailbox_append(output, &partial, sizeof(partial));
		static volatile uintptr_t address         = 8;
		*reinterpret_cast<volatile int*>(address) = 1;
		return 0;
	}

	constexpr qsim_image_api_t broken{QSIM_IMAGE_ABI_VERSION, bind, fault};
	constexpr qsim_image_api_t working{QSIM_IMAGE_ABI_VERSION, bind, healthy};

	TEST(CrashContainment, DiscardsOutputAndContinuesHealthyGrants) {
		using module::ModuleId;
		const std::array           bindings{firmware::ImageBinding{ModuleId{1}, &broken},
											firmware::ImageBinding{ModuleId{2}, &working}};
		firmware::InProcessBackend backend(bindings, 1, {});
		std::array grants{firmware::ExecutionGrant{.module = ModuleId{1}, .granted = true, .delta_us = 1},
						  firmware::ExecutionGrant{.module = ModuleId{2}, .granted = true, .delta_us = 1}};

		backend.execute_epoch(grants);
		ASSERT_TRUE(grants[0].crash);
		EXPECT_EQ(quercus_mailbox_count(&grants[0].outbound), 0);
		EXPECT_FALSE(grants[1].crash);
		EXPECT_EQ(quercus_mailbox_count(&grants[1].outbound), 1);
		const auto report = firmware::crash::report(*grants[0].crash, 1, 0, grants[0].module_time_us);
		EXPECT_NE(report.find("test_crash_containment.cpp:"), std::string::npos) << report;

		grants[0].granted = false;
		backend.execute_epoch(grants);
		EXPECT_FALSE(grants[0].crash);
		unsigned       size = 0;
		const uint8_t* data = quercus_mailbox_read(&grants[1].outbound, 0, &size);
		ASSERT_EQ(size, 1);
		ASSERT_NE(data, nullptr);
		EXPECT_EQ(*data, 2);
	}
} // namespace
