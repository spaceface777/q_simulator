#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

#include "capture.h"

namespace {

	class CapturingListener : public ::testing::EmptyTestEventListener {
	public:
		CapturingListener() : stdout_(STDOUT_FILENO), stderr_(STDERR_FILENO) {}

	private:
		void OnTestStart(const ::testing::TestInfo&) override {
			stdout_.start();
			stderr_.start();
		}

		void OnTestEnd(const ::testing::TestInfo& test_info) override {
			const std::string stdout_capture = stdout_.stop();
			const std::string stderr_capture = stderr_.stop();

			if (test_info.result()->Failed()) {
				if (!stdout_capture.empty()) {
					std::printf("----- captured stdout -----\n%s\n", stdout_capture.c_str());
				}
				if (!stderr_capture.empty()) {
					std::printf("----- captured stderr -----\n%s\n", stderr_capture.c_str());
				}
			}
		}

		FileDescriptorCapture stdout_;
		FileDescriptorCapture stderr_;
	};

} // namespace

int main(int argc, char** argv) {
	// testing::TempDir() falls back to /tmp/ when neither TEST_TMPDIR nor TMPDIR is set. Windows has
	// no /tmp, but the standard library also finds the temporary directory through TMP and TEMP.
	if (std::getenv("TEST_TMPDIR") == nullptr && std::getenv("TMPDIR") == nullptr) {
		std::error_code error;
		const auto      directory = std::filesystem::temp_directory_path(error);
		if (!error) {
			::setenv("TEST_TMPDIR", directory.string().c_str(), 0);
		}
	}
	::testing::InitGoogleTest(&argc, argv);
	::testing::UnitTest::GetInstance()->listeners().Append(new CapturingListener);
	return RUN_ALL_TESTS();
}
