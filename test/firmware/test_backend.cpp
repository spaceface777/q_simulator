#include "../../src/firmware/inprocess_backend.h"
#include "../../src/rpc/frame.h"
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
			setenv("TEST_APP_GRANT_RECORD", record.c_str(), 1);
		}

		~Inputs() {
			unsetenv("TEST_APP_GRANT_RECORD");
			unsetenv("TEST_APP_IDLE_UNTIL");
			unsetenv("TEST_APP_EXIT_AFTER");
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
	setenv("TEST_APP_IDLE_UNTIL", "10000", 1);
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

TEST(FirmwareBackend, FailureNamesModuleOperationOutcomeAndDiagnostic) {
	Inputs inputs;
	setenv("TEST_APP_EXIT_AFTER", "1", 1);
	firmware::InProcessBackend backend(std::span(&binding, 1), 41);
	firmware::ExecutionGrant   grant;
	prepare(grant);
	backend.execute_epoch(std::span(&grant, 1));
	prepare(grant);
	try {
		backend.execute_epoch(std::span(&grant, 1));
		FAIL() << "Expected firmware failure";
	} catch (const std::runtime_error& error) {
		const std::string message = error.what();
		for (const char* text : {"Module 1", "run", "outcome", "scripted exit"})
			EXPECT_NE(message.find(text), std::string::npos) << message;
	}
}
