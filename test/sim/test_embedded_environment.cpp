#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../file_cleanup.h"
#include "../os.h"
#include "firmware_images.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <sstream>
#include <unistd.h>

TEST(Runtime, GuestEnvironmentSnapshotSurvivesHostChangesAndFreshLoads) {
	if (test_applets::image_path("environment_probe", true).empty()) GTEST_SKIP() << "Missing environment_probe.dll";

	struct Restore {
		std::optional<std::string> value;

		~Restore() {
			if (value) test_os::set_environment("QSIM_STUB_REFUSE", value->c_str());
			else test_os::unset_environment("QSIM_STUB_REFUSE");
			for (uint8_t id : {1, 2}) settings::set_module_settings(module::ModuleId{id}, {});
		}
	} restore;

	if (const char* value = std::getenv("QSIM_STUB_REFUSE")) restore.value = value;
	for (unsigned repeat = 0; repeat != 2; ++repeat) {
		settings::install_test_defaults();
		for (uint8_t id = 1; id < MAX_MODULES; ++id) settings::set_module_settings(module::ModuleId{id}, {});
		settings::set_module_settings(module::ModuleId{255}, {});
		for (uint8_t id : {1, 2}) {
			settings::ModuleSettings module{};
			module.active = true;
			settings::set_module_settings(module::ModuleId{id}, module);
		}
		test_os::set_environment("QSIM_STUB_REFUSE", "host-only");
		std::map<std::string, std::string> environment{{"QSIM_STUB_REFUSE", "6:30000"}, {"EXTRA", "quoted \"value\""}};
		const auto                         trace =
			std::filesystem::temp_directory_path() / ("qsim-environment-" + std::to_string(getpid()) + ".ndjson");
		const test_util::FileCleanup cleanup_trace{trace};
		sim::Runtime                 runtime(42, *test_applets::bindings("environment_probe", true), environment);
		environment["QSIM_STUB_REFUSE"] = "caller-changed";
		runtime.trace_to(trace.string(), {});
		ASSERT_NO_THROW(runtime.start());
		test_os::set_environment("QSIM_STUB_REFUSE", "changed-after-start");
		while (runtime.tick() < 3000) {
			const auto before = runtime.tick();
			runtime.advance(std::chrono::microseconds(1000), 1.0, std::chrono::milliseconds(50));
			ASSERT_GT(runtime.tick(), before);
		}
		for (uint8_t id : {1, 2}) EXPECT_EQ(runtime.world().module(module::ModuleId{id})->led_color().raw(), 0x00ff00u);
		runtime.stop();
		std::ifstream input(trace);
		std::string   first;
		std::getline(input, first);
		// JSON is a separate syntax; inspect the complete, escaped map in the run_start record.
		EXPECT_NE(first.find("\"environment\":{\"EXTRA\":\"quoted \\\"value\\\"\",\"QSIM_STUB_REFUSE\":\"6:30000\"}"),
				  std::string::npos)
			<< first;
		EXPECT_EQ(first.find("host-only"), std::string::npos);
		EXPECT_EQ(first.find("caller-changed"), std::string::npos);
	}
}
