#include "os.h"

#include <cerrno>
#include <cstdlib>
#include <system_error>

namespace test_os {
	void set_environment(const char* name, const char* value) {
		if (setenv(name, value, 1)) throw std::system_error(errno, std::generic_category(), "setenv");
	}

	void unset_environment(const char* name) {
		if (unsetenv(name)) throw std::system_error(errno, std::generic_category(), "unsetenv");
	}
} // namespace test_os
