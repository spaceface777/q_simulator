#include "../../src/firmware/image.h"
#include "counter_api.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <set>
#include <string>

namespace {
	std::set<std::filesystem::path> copies() {
		std::set<std::filesystem::path> result;
		for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
			if (entry.path().filename().string().starts_with("qsim-image-")) result.insert(entry.path());
		return result;
	}

	uint64_t next(firmware::LoadedImage& image) {
		const auto*   api = static_cast<const CounterApi*>(image.api());
		CounterResult result{};
		EXPECT_EQ(api->run(nullptr, &result), 0);
		return result.next_wake_us;
	}

	void expect_failure(const std::filesystem::path& path, const std::string& reason) {
		const auto before = copies();
		try {
			firmware::LoadedImage image(path);
			FAIL() << "Loaded invalid image " << path;
		} catch (const std::runtime_error& error) {
			const std::string message = error.what();
			EXPECT_NE(message.find(path.string()), std::string::npos) << message;
			EXPECT_NE(message.find(reason), std::string::npos) << message;
		}
		EXPECT_EQ(copies(), before);
	}
} // namespace

TEST(FirmwareImage, PrivateCopiesHaveIndependentGlobals) {
	const auto before = copies();
	{
		firmware::LoadedImage a(COUNTER_IMAGE), b(COUNTER_IMAGE);
		EXPECT_NE(a.api(), b.api());
		EXPECT_EQ(next(a), 1u);
		EXPECT_EQ(next(a), 2u);
		EXPECT_EQ(next(b), 1u);
		EXPECT_EQ(next(a), 3u);
		EXPECT_EQ(next(b), 2u);
	}
	EXPECT_EQ(copies(), before);
}

TEST(FirmwareImage, ReloadStartsWithFreshGlobals) {
	const auto before = copies();
	for (unsigned round = 0; round < 3; ++round) {
		firmware::LoadedImage image(COUNTER_IMAGE);
		EXPECT_EQ(next(image), 1u);
		EXPECT_EQ(next(image), 2u);
	}
	EXPECT_EQ(copies(), before);
}

TEST(FirmwareImage, MissingFileNamesPathAndReason) {
	expect_failure(std::string(COUNTER_IMAGE) + ".missing", "copy_file");
}

TEST(FirmwareImage, NonImageReportsLoaderError) {
	expect_failure(NON_IMAGE, "load library:");
}

TEST(FirmwareImage, WrongVersionNamesActualAndExpectedVersions) {
	expect_failure(WRONG_ABI_IMAGE, "ABI version 9; expected 10");
}

TEST(FirmwareImage, MissingEntryReportsSymbolError) {
	expect_failure(MISSING_ENTRY_IMAGE, "find qsim_image_api:");
}

TEST(FirmwareImage, NullApiReportsReason) {
	expect_failure(NULL_API_IMAGE, "qsim_image_api returned null");
}
