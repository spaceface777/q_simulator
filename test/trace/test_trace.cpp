#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../../src/seed/seed.h"
#include "../../src/trace/trace.h"

namespace {

	namespace fs = std::filesystem;

	fs::path scratch_file(const std::string& name) {
		const auto* test = ::testing::UnitTest::GetInstance()->current_test_info();
		return fs::path(::testing::TempDir()) /
			   (std::string(test->test_suite_name()) + "_" + test->name() + "_" + name);
	}

	std::string read_file(const fs::path& path) {
		std::ifstream     file(path, std::ios::binary);
		std::stringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}

	TEST(TraceEntry, WritesFieldsInInsertionOrder) {
		const auto entry = trace::Entry("tick")
							   .field("tick", uint64_t{18446744073709551615u})
							   .field("delta", int64_t{-42})
							   .field("speed", 1.0 / 3.0)
							   .field("active", true)
							   .field("blocked", false)
							   .field("label", std::string("plain"));
		EXPECT_EQ(entry.str(), "{\"type\":\"tick\",\"tick\":18446744073709551615,\"delta\":-42,"
							   "\"speed\":0.333333,\"active\":true,\"blocked\":false,\"label\":\"plain\"}");
	}

	TEST(TraceEntry, EscapesQuotesBackslashesAndControlCharacters) {
		const std::string value = std::string("say \"hi\"\\ \n\r\t") + '\x01' + '\x1f' + " end";
		const auto        entry = trace::Entry("message").field("text", value);
		EXPECT_EQ(entry.str(), "{\"type\":\"message\",\"text\":\"say \\\"hi\\\"\\\\ \\n\\r\\t\\u0001\\u001f end\"}");
	}

	TEST(TraceEntry, EscapesTypeAndKeys) {
		const auto entry = trace::Entry("a\"b").field("k\\ey", uint64_t{1});
		EXPECT_EQ(entry.str(), "{\"type\":\"a\\\"b\",\"k\\\\ey\":1}");
	}

	TEST(TraceEntry, KeepsBytesFromDelAndAbove) {
		const auto entry = trace::Entry("utf8").field("text", std::string("\x7f\xc3\xa9"));
		EXPECT_EQ(entry.str(), "{\"type\":\"utf8\",\"text\":\"\x7f\xc3\xa9\"}");
	}

	TEST(TraceEntry, WritesMapsSortedAndEscaped) {
		const std::map<std::string, std::string> values{{"z", "last"}, {"a\"", "first\n"}};
		const auto                               entry =
			trace::Entry("labels").field("values", values).field("empty", std::map<std::string, std::string>{});
		EXPECT_EQ(entry.str(), "{\"type\":\"labels\",\"values\":{\"a\\\"\":\"first\\n\",\"z\":\"last\"},\"empty\":{}}");
	}

	TEST(TraceWriter, EmptyPathDisablesWriting) {
		trace::Writer writer{fs::path{}};
		EXPECT_FALSE(writer.enabled());
		writer.write(trace::Entry("ignored"));
		writer.flush();
		EXPECT_EQ(writer.count(), 0u);
	}

	TEST(TraceWriter, WritesOneLinePerEntryAndTruncates) {
		const fs::path path = scratch_file("trace.ndjson");
		{
			std::ofstream stale(path);
			stale << "stale contents that are longer than the trace\n";
		}
		{
			trace::Writer writer(path);
			EXPECT_TRUE(writer.enabled());
			writer.write(trace::Entry("first").field("tick", uint64_t{1}));
			writer.write(trace::Entry("second"));
			EXPECT_EQ(writer.count(), 2u);
			writer.flush();
			EXPECT_EQ(read_file(path), "{\"type\":\"first\",\"tick\":1}\n{\"type\":\"second\"}\n");
			writer.write(trace::Entry("third"));
		}
		// The destructor flushes the last entry.
		EXPECT_EQ(read_file(path), "{\"type\":\"first\",\"tick\":1}\n{\"type\":\"second\"}\n{\"type\":\"third\"}\n");
		fs::remove(path);
	}

	TEST(TraceWriter, ThrowsWhenTheFileCannotBeOpened) {
		const fs::path path = scratch_file("missing") / "trace.ndjson";
		try {
			trace::Writer writer(path);
			FAIL() << "opened " << path;
		} catch (const std::runtime_error& error) {
			EXPECT_EQ(std::string(error.what()), "Failed to open the trace file at " + path.string());
		}
	}

	TEST(TraceDigest, HashesContentsOrReportsMissing) {
		const fs::path path = scratch_file("digest.txt");
		EXPECT_EQ(trace::digest_file(path), "missing");
		{
			std::ofstream file(path, std::ios::binary);
			file << "abc";
		}
		EXPECT_EQ(trace::digest_file(path), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		EXPECT_EQ(trace::digest_file(path), seed::sha256_hex("abc"));
		fs::remove(path);
	}

} // namespace
