// Names the frames of a crash capture with libbacktrace: function, source file and line, and
// inlined calls. Falls back to symbol tables, then to an offset within the library.
#include "crash_symbols.h"

#include <backtrace.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// crash_backtrace_memory.c: frees everything that libbacktrace allocated.
extern "C" void qsim_backtrace_free_all(void);

namespace firmware::crash {
	namespace {
		std::atomic<uint64_t> loaded_generation{0};
		std::mutex            symbolizer; // libbacktrace's memory is not thread-safe

		// The first error that libbacktrace reported during this report, and whether its state
		// must be rebuilt: a state that failed to read the loaded objects never tries again.
		// Guarded by `symbolizer`.
		std::string problem;
		bool        state_failed = false;

		void record_error(void*, const char* message, int number) {
			// -1: an object has no debug information. The report names those objects itself.
			if (number == -1 || !problem.empty()) return;
			problem = message ? message : "unknown error";
			if (number > 0) problem += std::string(": ") + std::strerror(number);
		}

		// libbacktrace reads the list of loaded objects once per state, so images loaded later
		// need a new state. It cannot free a state itself: freeing all of its memory replaces the
		// previous one.
		backtrace_state* debug_state() {
			static backtrace_state* state      = nullptr;
			static uint64_t         generation = 0;
			const uint64_t          current    = loaded_generation.load(std::memory_order_acquire);
			if (state == nullptr || generation != current || state_failed) {
				qsim_backtrace_free_all();
				state        = backtrace_create_state(nullptr, 0, record_error, nullptr);
				generation   = current;
				state_failed = false;
			}
			return state;
		}

		std::string demangle(const char* name) {
			if (name == nullptr) return {};
			int                                         status = 0;
			std::unique_ptr<char, decltype(&std::free)> plain(abi::__cxa_demangle(name, nullptr, nullptr, &status),
															  &std::free);
			return status == 0 && plain ? std::string(plain.get()) : std::string(name);
		}

		struct ObjectInfo {
			std::string path;
			uintptr_t   base       = 0;
			bool        executable = false;
			std::string exported_symbol; // The nearest exported symbol, if the loader knows one
		};

		ObjectInfo object_of(uintptr_t address) {
			ObjectInfo object;
#ifdef _WIN32
			HMODULE module = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
									   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								   reinterpret_cast<LPCSTR>(address), &module)) {
				char        path[MAX_PATH * 4];
				const DWORD length = GetModuleFileNameA(module, path, sizeof(path));
				object.path        = std::string(path, length);
				object.base        = reinterpret_cast<uintptr_t>(module);
				object.executable  = module == GetModuleHandleA(nullptr);
			}
#else
			static const void* executable_base = [] {
				Dl_info own{};
				return dladdr(reinterpret_cast<const void*>(&images_changed), &own) ? own.dli_fbase : nullptr;
			}();
			Dl_info info{};
			if (dladdr(reinterpret_cast<const void*>(address), &info) && info.dli_fname) {
				object.path       = info.dli_fname;
				object.base       = reinterpret_cast<uintptr_t>(info.dli_fbase);
				object.executable = info.dli_fbase == executable_base;
				if (info.dli_sname) object.exported_symbol = info.dli_sname;
			}
#endif
			return object;
		}

		std::string canonical(const std::string& path) {
			std::error_code error;
			const auto      resolved = std::filesystem::weakly_canonical(path, error);
			return error ? path : resolved.string();
		}

		struct Lines {
			std::vector<SymbolizedFrame> frames;
		};

		// Line 0 marks code that the compiler merged from several lines, often identical calls
		// to a function that does not return. The file and function are still right.
		int on_line(void* data, uintptr_t, const char* file, int line, const char* function) {
			if (file == nullptr || (line <= 0 && function == nullptr)) return 0;
			// Clang describes a failed check as an inlined function named after its reason. The
			// fault description already says what the check was.
			if (function != nullptr && std::strncmp(function, "__clang_trap_msg$", 17) == 0) return 0;
			auto&           lines = *static_cast<Lines*>(data);
			SymbolizedFrame frame;
			frame.function = demangle(function);
			frame.file     = std::filesystem::path(file).lexically_normal().string();
			frame.line     = std::max(line, 0);
			lines.frames.push_back(std::move(frame));
			return 0;
		}

		struct Symbol {
			std::string name;
			uintptr_t   start = 0;
		};

		void on_symbol(void* data, uintptr_t, const char* name, uintptr_t start, uintptr_t) {
			auto& symbol = *static_cast<Symbol*>(data);
			if (name) {
				symbol.name  = demangle(name);
				symbol.start = start;
			}
		}

		Symbol symbol_at(backtrace_state* state, uintptr_t address) {
			Symbol symbol;
			if (state) backtrace_syminfo(state, address, on_symbol, record_error, &symbol);
			return symbol;
		}
	} // namespace

	void images_changed() noexcept {
		loaded_generation.fetch_add(1, std::memory_order_acq_rel);
	}

	Symbolization symbolize(const Capture& capture, const std::vector<ImageName>& images) {
		const std::lock_guard lock(symbolizer);
		problem.clear();
		backtrace_state* state = debug_state();
		if (state == nullptr && problem.empty()) problem = "cannot create the libbacktrace state";

		std::map<std::string, std::string> names;
		for (const ImageName& image : images) names.emplace(canonical(image.loaded_path), image.display);

		// frames[0] is the faulting instruction itself, except when the image's fatal hook recorded
		// the stack: then every entry is a return address, which points after its call.
		std::vector<uintptr_t> addresses(capture.frames.begin(), capture.frames.begin() + capture.frame_count);
		const bool             exact_first = capture.cause != Cause::ENDED;
		std::size_t            gap         = capture.omitted ? capture.omitted_at : SIZE_MAX;

		// A leaf function, or a call through an invalid pointer, has not saved its return address
		// in the frame chain. The arm64 link register or the word at the stack pointer holds it.
		if (exact_first && capture.caller != 0 && !addresses.empty() &&
			(addresses.size() < 2 || addresses[1] != capture.caller)) {
			bool insert = !capture.has_instruction;
#if defined(__aarch64__)
			if (!insert) {
				const Symbol at_pc     = symbol_at(state, addresses[0]);
				const Symbol at_caller = symbol_at(state, capture.caller - 1);
				insert                 = at_pc.start != 0 && at_caller.start != 0 && at_pc.start != at_caller.start;
			}
#endif
			if (insert && object_of(capture.caller - 1).base != 0) {
				addresses.insert(addresses.begin() + 1, capture.caller);
				if (gap != SIZE_MAX && gap >= 1) gap++;
			}
		}

		std::vector<SymbolizedFrame> result;
		for (std::size_t index = 0; index < addresses.size(); index++) {
			const uintptr_t address = addresses[index];
			// A return address follows its call. Look up the call itself.
			const uintptr_t lookup = index == 0 && exact_first ? address : address - 1;

			const ObjectInfo object = object_of(lookup);
			std::string      display;
			bool             application = false;
			if (!object.path.empty()) {
				const auto found = names.find(canonical(object.path));
				application      = found != names.end();
				display          = application ? found->second : std::filesystem::path(object.path).filename().string();
			}

			Lines lines;
			if (state) backtrace_pcinfo(state, lookup, on_line, record_error, &lines);
			if (lines.frames.empty()) {
				SymbolizedFrame frame;
				frame.function = symbol_at(state, lookup).name;
				if (frame.function.empty() && !application) frame.function = demangle(object.exported_symbol.c_str());
				lines.frames.push_back(std::move(frame));
			}

			for (std::size_t entry = 0; entry < lines.frames.size(); entry++) {
				SymbolizedFrame& frame   = lines.frames[entry];
				frame.address            = address;
				frame.inlined            = entry > 0;
				frame.object             = display;
				frame.object_offset      = object.base ? lookup - object.base : 0;
				frame.in_host_executable = object.executable;
				frame.in_application     = application;
				frame.omitted_before     = index == gap && entry == 0 ? capture.omitted : 0;
				result.push_back(std::move(frame));
			}
		}
		state_failed = !problem.empty();
		return {std::move(result), problem};
	}

} // namespace firmware::crash
