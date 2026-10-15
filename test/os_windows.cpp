#include "os.h"

#include <cstdlib>
#include <system_error>

namespace test_os {
	void set_environment(const char* name, const char* value) {
		if (const int error = _putenv_s(name, value))
			throw std::system_error(error, std::generic_category(), "_putenv_s");
	}

	void unset_environment(const char* name) {
		set_environment(name, "");
	}
} // namespace test_os
