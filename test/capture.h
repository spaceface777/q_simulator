#pragma once

#include <cstdio>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

// Captures what a program writes to one of its file descriptors, through a file from std::tmpfile.
// Use it instead of GoogleTest's CaptureStdout and CaptureStderr: outside Windows builds they make
// their file in /tmp, which a Cosmopolitan program running on Windows doesn't have.
class FileDescriptorCapture {
public:
	explicit FileDescriptorCapture(int target) : target_(target) {}

	FileDescriptorCapture(const FileDescriptorCapture&)            = delete;
	FileDescriptorCapture& operator=(const FileDescriptorCapture&) = delete;

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

// Drop-in replacements for testing::internal::CaptureStdout() and GetCapturedStdout(), and the
// same for stderr.
namespace capture {

	inline FileDescriptorCapture& stdout_capture() {
		static FileDescriptorCapture capture(STDOUT_FILENO);
		return capture;
	}

	inline FileDescriptorCapture& stderr_capture() {
		static FileDescriptorCapture capture(STDERR_FILENO);
		return capture;
	}

	inline void start_stdout() {
		stdout_capture().start();
	}

	inline std::string stop_stdout() {
		return stdout_capture().stop();
	}

	inline void start_stderr() {
		stderr_capture().start();
	}

	inline std::string stop_stderr() {
		return stderr_capture().stop();
	}

} // namespace capture
