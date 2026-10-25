#include "../../src/firmware/image.h"
#include "counter_api.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <set>
#include <string>

namespace {
	void set_cache(const std::filesystem::path& path) {
#ifdef _WIN32
		_putenv_s("QSIM_IMAGE_CACHE", path.string().c_str());
#else
		setenv("QSIM_IMAGE_CACHE", path.c_str(), 1);
#endif
	}

	void clear_cache_setting() {
#ifdef _WIN32
		_putenv_s("QSIM_IMAGE_CACHE", "");
#else
		unsetenv("QSIM_IMAGE_CACHE");
#endif
	}

	void expect_failure(const std::filesystem::path& path, const std::string& reason);

	// Each test uses its own empty image cache.
	class FirmwareImage : public ::testing::Test {
	protected:
		void SetUp() override {
			const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
			root_ = std::filesystem::temp_directory_path() / ("qsim-image-cache-test-" + std::string(test->name()));
			std::filesystem::remove_all(root_);
			set_cache(root_);
		}

		void TearDown() override {
			clear_cache_setting();
			std::error_code ignored;
			std::filesystem::remove_all(root_, ignored);
		}

		// Cached copies relative to the cache root, such as "<prefix>/<digest-tail>/0.dll".
		std::set<std::string> copies() const {
			std::set<std::string> result;
			if (!std::filesystem::exists(root_)) return result;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(root_))
				if (entry.is_regular_file()) result.insert(entry.path().lexically_relative(root_).generic_string());
			return result;
		}

		// Every failed load releases its slot, so repeated failures reuse one copy.
		void expect_repeated_failure(const std::filesystem::path& path, const std::string& reason) const {
			expect_failure(path, reason);
			expect_failure(path, reason);
			const auto cached = copies();
			EXPECT_EQ(cached.size(), 1u);
			for (const auto& copy : cached) EXPECT_TRUE(copy.ends_with("/0.dll")) << copy;
		}

		std::filesystem::path root_;
	};

	uint64_t next(firmware::LoadedImage& image) {
		const auto*   api = static_cast<const CounterApi*>(image.api());
		CounterResult result{};
		EXPECT_EQ(api->run(nullptr, &result), 0);
		return result.next_wake_us;
	}

	void expect_failure(const std::filesystem::path& path, const std::string& reason) {
		try {
			firmware::LoadedImage image(path);
			FAIL() << "Loaded invalid image " << path;
		} catch (const std::runtime_error& error) {
			const std::string message = error.what();
			EXPECT_NE(message.find(path.string()), std::string::npos) << message;
			EXPECT_NE(message.find(reason), std::string::npos) << message;
		}
	}
} // namespace

TEST_F(FirmwareImage, PrivateCopiesHaveIndependentGlobals) {
	{
		firmware::LoadedImage a(COUNTER_IMAGE), b(COUNTER_IMAGE);
		EXPECT_NE(a.api(), b.api());
		EXPECT_EQ(next(a), 1u);
		EXPECT_EQ(next(a), 2u);
		EXPECT_EQ(next(b), 1u);
		EXPECT_EQ(next(a), 3u);
		EXPECT_EQ(next(b), 2u);
	}
	const auto cached = copies();
	ASSERT_EQ(cached.size(), 2u);
	EXPECT_TRUE(cached.begin()->ends_with("/0.dll"));
	EXPECT_TRUE(cached.rbegin()->ends_with("/1.dll"));
}

TEST_F(FirmwareImage, ReloadStartsWithFreshGlobals) {
	for (unsigned round = 0; round < 3; ++round) {
		firmware::LoadedImage image(COUNTER_IMAGE);
		EXPECT_EQ(next(image), 1u);
		EXPECT_EQ(next(image), 2u);
	}
	EXPECT_EQ(copies().size(), 1u);
}

TEST_F(FirmwareImage, CachedCopyIsReusedUnchanged) {
	{
		firmware::LoadedImage image(COUNTER_IMAGE);
	}
	const auto copy  = root_ / *copies().begin();
	const auto stamp = std::filesystem::last_write_time(copy);
	{
		firmware::LoadedImage image(COUNTER_IMAGE);
		EXPECT_EQ(next(image), 1u);
	}
	EXPECT_EQ(std::filesystem::last_write_time(copy), stamp);
}

TEST_F(FirmwareImage, MissingCopyIsRecreated) {
	{
		firmware::LoadedImage image(COUNTER_IMAGE);
	}
	const auto copy = root_ / *copies().begin();
	std::filesystem::remove(copy);
	firmware::LoadedImage image(COUNTER_IMAGE);
	EXPECT_EQ(next(image), 1u);
	EXPECT_EQ(std::filesystem::file_size(copy), std::filesystem::file_size(COUNTER_IMAGE));
}

TEST_F(FirmwareImage, CacheKeepsOnlyRecentImages) {
	const auto sources = root_.parent_path() / (root_.filename().string() + "-sources");
	std::filesystem::create_directories(sources);
	for (unsigned index = 0; index < 10; ++index) {
		const auto variant = sources / ("variant-" + std::to_string(index) + ".dll");
		std::ofstream(variant, std::ios::binary) << "not an image " << index;
		expect_failure(variant, "load library");
	}
	std::filesystem::remove_all(sources);
	std::set<std::string> images;
	for (const auto& copy : copies()) images.insert(std::filesystem::path(copy).parent_path().generic_string());
	EXPECT_EQ(images.size(), 8u);
}

TEST_F(FirmwareImage, MissingFileNamesPathAndReason) {
	expect_failure(std::string(COUNTER_IMAGE) + ".missing", "read");
	EXPECT_TRUE(copies().empty());
}

TEST_F(FirmwareImage, NonImageReportsLoaderError) {
	expect_repeated_failure(NON_IMAGE, "load library:");
}

TEST_F(FirmwareImage, WrongVersionNamesActualAndExpectedVersions) {
	expect_repeated_failure(WRONG_ABI_IMAGE, "ABI version 9; expected 10");
}

TEST_F(FirmwareImage, MissingEntryReportsSymbolError) {
	expect_repeated_failure(MISSING_ENTRY_IMAGE, "find qsim_image_api:");
}

TEST_F(FirmwareImage, NullApiReportsReason) {
	expect_repeated_failure(NULL_API_IMAGE, "qsim_image_api returned null");
}
