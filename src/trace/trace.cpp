#include "trace.h"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

#include "../seed/seed.h"

namespace trace {

	namespace {

		// Only needs to handle labels and messages; payloads are recorded as hexadecimal elsewhere.
		std::string escape(const std::string& value) {
			std::string out;
			out.reserve(value.size() + 2);
			for (const char character : value) {
				switch (character) {
				case '"':  out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (static_cast<unsigned char>(character) < 0x20) {
						char buffer[8];
						std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(character));
						out += buffer;
					} else {
						out += character;
					}
					break;
				}
			}
			return out;
		}

	} // namespace

	Entry::Entry(const std::string& type) {
		body_ = "\"type\":\"" + escape(type) + "\"";
	}

	void Entry::separate() {
		if (!body_.empty()) {
			body_ += ",";
		}
	}

	Entry& Entry::field(const std::string& key, const std::string& value) {
		separate();
		body_ += "\"" + escape(key) + "\":\"" + escape(value) + "\"";
		return *this;
	}

	Entry& Entry::field(const std::string& key, uint64_t value) {
		separate();
		body_ += "\"" + escape(key) + "\":" + std::to_string(value);
		return *this;
	}

	Entry& Entry::field(const std::string& key, int64_t value) {
		separate();
		body_ += "\"" + escape(key) + "\":" + std::to_string(value);
		return *this;
	}

	Entry& Entry::field(const std::string& key, double value) {
		separate();
		// Fixed precision so a value identical in the engine stays identical in the file.
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%.6f", value);
		body_ += "\"" + escape(key) + "\":" + buffer;
		return *this;
	}

	Entry& Entry::field(const std::string& key, bool value) {
		separate();
		body_ += "\"" + escape(key) + "\":" + (value ? "true" : "false");
		return *this;
	}

	Entry& Entry::field(const std::string& key, const std::map<std::string, std::string>& value) {
		separate();
		body_ += "\"" + escape(key) + "\":{";
		bool first = true;
		for (const auto& [name, text] : value) {
			if (!first) body_ += ',';
			first = false;
			body_ += "\"" + escape(name) + "\":\"" + escape(text) + "\"";
		}
		body_ += '}';
		return *this;
	}

	std::string Entry::str() const {
		return "{" + body_ + "}";
	}

	Writer::Writer(const fs::path& path) {
		if (path.empty()) {
			return;
		}

		file_.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
		if (!file_.is_open()) {
			throw std::runtime_error("Failed to open the trace file at " + path.string());
		}
		enabled_ = true;
	}

	Writer::~Writer() {
		if (enabled_) {
			file_.flush();
		}
	}

	void Writer::write(const Entry& entry) {
		if (!enabled_) {
			return;
		}
		// The runtime flushes every 100 ticks and at shutdown. A full stream buffer can write sooner.
		file_ << entry.str() << "\n";
		written_++;
	}

	void Writer::flush() {
		if (enabled_) {
			file_.flush();
		}
	}

	std::string digest_file(const fs::path& path) {
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) {
			return "missing";
		}

		std::stringstream buffer;
		buffer << file.rdbuf();
		return seed::sha256_hex(buffer.str());
	}

} // namespace trace
