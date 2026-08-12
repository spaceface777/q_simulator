#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "../../src/childproc/launcher.h"

namespace fs = std::filesystem;

namespace {

	// A script that records its launch environment and arguments, standing in for a real applet.
	fs::path write_recording_script(const fs::path& output) {
		const fs::path script = fs::temp_directory_path() / ("quercus_launch_" + std::to_string(::getpid()) + ".sh");
		std::ofstream  file(script);
		file << "#!/bin/sh\n";
		file << "printf '%s|%s|%s|%s|%s\\n' \"$QSIM_CTRL_FD\" \"$QSIM_MBOX_FD\" \"$QSIM_MODULE_ID\" \"$1\" \"$2\" > "
			 << output << "\n";
		file.close();
		fs::permissions(script, fs::perms::owner_all);
		return script;
	}

	std::string wait_for_contents(const fs::path& path) {
		for (int attempt = 0; attempt < 500; attempt++) {
			if (fs::exists(path) && fs::file_size(path) > 0) {
				std::ifstream     file(path);
				std::stringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			}
			::usleep(2000);
		}
		return "";
	}

} // namespace

TEST(LauncherTest, PassesEndpointAndRunnerOptionsToTheChild) {
	const fs::path output = fs::temp_directory_path() / ("quercus_launch_out_" + std::to_string(::getpid()) + ".txt");
	fs::remove(output);
	const fs::path script = write_recording_script(output);

	childproc::LaunchSpec spec;
	spec.id         = module::ModuleId{7};
	spec.binary     = script.string();
	spec.control_fd = 11;
	spec.mailbox_fd = 12;
	spec.seed       = 42;

	childproc::Handle handle = childproc::launch(spec);
	EXPECT_GT(handle.pid(), 0);

	const std::string recorded = wait_for_contents(output);
	EXPECT_EQ(recorded, "11|12|7|--no-rt|--seed=42\n");

	fs::remove(script);
	fs::remove(output);
}

TEST(LauncherTest, ReportsExitStatusOnce) {
	childproc::LaunchSpec spec;
	spec.id     = module::ModuleId{1};
	spec.binary = "/bin/true";

	childproc::Handle handle = childproc::launch(spec);

	std::optional<int> status;
	for (int attempt = 0; attempt < 500 && !status.has_value(); attempt++) {
		status = handle.exit_status();
		if (!status.has_value()) {
			::usleep(2000);
		}
	}

	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(status.value(), 0);
	// The status stays available after the child has been reaped.
	EXPECT_EQ(handle.exit_status().value(), 0);
}

TEST(LauncherTest, ReportsFailureToExec) {
	childproc::LaunchSpec spec;
	spec.id     = module::ModuleId{1};
	spec.binary = "/nonexistent/quercus/application";

	childproc::Handle handle = childproc::launch(spec);

	std::optional<int> status;
	for (int attempt = 0; attempt < 500 && !status.has_value(); attempt++) {
		status = handle.exit_status();
		if (!status.has_value()) {
			::usleep(2000);
		}
	}

	ASSERT_TRUE(status.has_value());
	EXPECT_EQ(status.value(), 127);
}

TEST(LauncherTest, TerminateKillsARunningChild) {
	childproc::LaunchSpec spec;
	spec.id     = module::ModuleId{1};
	spec.binary = "/bin/sleep";

	childproc::Handle handle = childproc::launch(spec);
	const pid_t       pid    = handle.pid();
	ASSERT_GT(pid, 0);

	handle.terminate();

	EXPECT_EQ(handle.pid(), -1);
	EXPECT_EQ(::kill(pid, 0), -1);
}
