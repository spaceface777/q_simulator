#include "../../src/firmware/inprocess_backend.h"
#include "../../src/rpc/frame.h"
#include "../os.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <unistd.h>

namespace {
	struct Inputs {
		std::filesystem::path record =
			std::filesystem::temp_directory_path() / ("qsim-grants-" + std::to_string(getpid()));

		Inputs() {
			std::filesystem::remove(record);
			test_os::set_environment("TEST_APP_GRANT_RECORD", record.string().c_str());
		}

		~Inputs() {
			test_os::unset_environment("TEST_APP_GRANT_RECORD");
			test_os::unset_environment("TEST_APP_IDLE_UNTIL");
			test_os::unset_environment("TEST_APP_EXIT_AFTER");
			std::filesystem::remove(record);
		}
	};

	const firmware::ImageBinding binding{module::ModuleId{1}, SCRIPTED_IMAGE};

	void prepare(firmware::ExecutionGrant& grant, uint32_t delta = 1000) {
		grant.reset_metadata(module::ModuleId{1}, true);
		grant.granted  = true;
		grant.delta_us = delta;
	}
} // namespace

TEST(FirmwareBackend, SkipsIdleWindowsAndCatchesUpBeforeInput) {
	Inputs inputs;
	test_os::set_environment("TEST_APP_IDLE_UNTIL", "10000");
	firmware::InProcessBackend backend(std::span(&binding, 1), 41);
	firmware::ExecutionGrant   grant;
	prepare(grant);
	backend.execute_epoch(std::span(&grant, 1));
	ASSERT_EQ(grant.outbound.size(), 1u);
	for (unsigned i = 0; i < 3; ++i) {
		prepare(grant);
		backend.execute_epoch(std::span(&grant, 1));
		EXPECT_TRUE(grant.outbound.valid());
		EXPECT_EQ(grant.outbound.size(), 0u);
	}
	prepare(grant);
	rpc::Frame response;
	response.type    = MT_RESP;
	response.cmd     = CMD_HEARTBEAT;
	const auto bytes = rpc::encode(response);
	ASSERT_EQ(
		qsim_frame_batch_append(&grant.inbound, &grant.inbound_used, grant.inbound_count++, bytes.data(), bytes.size()),
		QSIM_BATCH_OK);
	backend.execute_epoch(std::span(&grant, 1));
	backend.stop();
	std::ifstream     file(inputs.record);
	std::stringstream contents;
	contents << file.rdbuf();
	EXPECT_EQ(contents.str(), "1:0:1000:0\n1:4000:5000:1\n");
}

TEST(FirmwareBackend, RejectsZeroLengthWithoutPublishingOldOutput) {
	Inputs                     inputs;
	firmware::InProcessBackend backend(std::span(&binding, 1), 41);
	firmware::ExecutionGrant   grant;
	prepare(grant);
	backend.execute_epoch(std::span(&grant, 1));
	ASSERT_EQ(grant.outbound.size(), 1u);
	prepare(grant, 0);
	EXPECT_THROW(backend.execute_epoch(std::span(&grant, 1)), std::runtime_error);
	EXPECT_EQ(grant.outbound.size(), 0u);
}

TEST(FirmwareBackend, FatalOutcomeStopsTheModuleInsteadOfTheEpoch) {
	Inputs inputs;
	test_os::set_environment("TEST_APP_EXIT_AFTER", "1");
	firmware::InProcessBackend backend(std::span(&binding, 1), 41);
	firmware::ExecutionGrant   grant;
	prepare(grant);
	backend.execute_epoch(std::span(&grant, 1));
	EXPECT_FALSE(grant.failure);
	prepare(grant);
	ASSERT_NO_THROW(backend.execute_epoch(std::span(&grant, 1)));
	ASSERT_TRUE(grant.failure);
	EXPECT_EQ(grant.failure->summary, "scripted exit");
	for (const char* text : {"Module 1", "scripted exit", "is stopped"})
		EXPECT_NE(grant.failure->report.find(text), std::string::npos) << grant.failure->report;
	EXPECT_EQ(grant.outbound.size(), 0u);
	// The application has stopped. Granting it again is a caller error.
	prepare(grant);
	EXPECT_THROW(backend.execute_epoch(std::span(&grant, 1)), std::runtime_error);
}
