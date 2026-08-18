#include <cstdio>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {

	class FileDescriptorCapture {
	public:
		explicit FileDescriptorCapture(int target) : target_(target) {}

		~FileDescriptorCapture() {
			if (saved_ >= 0) {
				std::fflush(nullptr);
				::dup2(saved_, target_);
				::close(saved_);
			}
			if (stream_ != nullptr) {
				std::fclose(stream_);
			}
		}

		void start() {
			std::fflush(nullptr);
			saved_  = ::dup(target_);
			stream_ = std::tmpfile();
			GTEST_CHECK_(saved_ >= 0 && stream_ != nullptr && ::dup2(::fileno(stream_), target_) >= 0);
		}

		std::string stop() {
			std::fflush(nullptr);
			GTEST_CHECK_(saved_ >= 0 && stream_ != nullptr);
			GTEST_CHECK_(::dup2(saved_, target_) >= 0);
			::close(saved_);
			saved_ = -1;

			GTEST_CHECK_(std::fseek(stream_, 0, SEEK_SET) == 0);
			std::string capture;
			char        buffer[4096];
			while (const std::size_t size = std::fread(buffer, 1, sizeof(buffer), stream_)) {
				capture.append(buffer, size);
			}
			std::fclose(stream_);
			stream_ = nullptr;
			return capture;
		}

	private:
		int   target_;
		int   saved_  = -1;
		FILE* stream_ = nullptr;
	};

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
	::testing::InitGoogleTest(&argc, argv);
	::testing::UnitTest::GetInstance()->listeners().Append(new CapturingListener);
	return RUN_ALL_TESTS();
}
