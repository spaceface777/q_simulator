#ifndef TRACE_TRACE_H
#define TRACE_TRACE_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../module/types.h"

namespace trace {

	namespace fs = std::filesystem;

	// Newline-delimited JSON record of a run. Two runs of one scenario produce identical files.

	// One entry, built field by field, written in insertion order for identical bytes.
	class Entry {
	private:
		std::string body_;

		void separate();

	public:
		explicit Entry(const std::string& type);

		Entry& field(const std::string& key, const std::string& value);
		Entry& field(const std::string& key, uint64_t value);
		Entry& field(const std::string& key, int64_t value);
		Entry& field(const std::string& key, double value);
		Entry& field(const std::string& key, bool value);

		// No trailing newline.
		[[nodiscard]] std::string str() const;
	};

	class Writer {
	private:
		std::ofstream file_;
		bool          enabled_ = false;
		std::size_t   written_ = 0;

	public:
		Writer() = default;

		// An empty path disables tracing. Throws std::runtime_error if the file cannot be opened.
		explicit Writer(const fs::path& path);

		~Writer();

		Writer(const Writer&)            = delete;
		Writer& operator=(const Writer&) = delete;
		Writer(Writer&&)                 = default;
		Writer& operator=(Writer&&)      = default;

		[[nodiscard]] bool enabled() const noexcept { return enabled_; }

		[[nodiscard]] std::size_t count() const noexcept { return written_; }

		void write(const Entry& entry);

		void flush();
	};

	// Returns the SHA-256 of the file's contents as hex, or "missing" if it does not exist.
	[[nodiscard]] std::string digest_file(const fs::path& path);

} // namespace trace

#endif // TRACE_TRACE_H
