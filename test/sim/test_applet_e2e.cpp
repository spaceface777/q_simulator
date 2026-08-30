#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/world/world.h"

namespace fs = std::filesystem;

namespace {

	std::string applet_path(const std::string& applet) {
		const fs::path path = fs::path(REPO_ROOT) / "external" / "q_applets" / "out" / (applet + ".exe");
		if (!fs::exists(path)) {
			return "";
		}
		return path.string();
	}

	class AppletTest : public ::testing::Test {
	protected:
		static constexpr uint8_t MODULE = 1;

		void SetUp() override {
			settings::install_test_defaults();
			world::set_calibration_config(world::CalibrationConfig{});
			world::set_calibration_override(world::Calibration::nominal());
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, settings::ModuleSettings{});
			settings::set_setting("network_fixed_delay_ms", 5);
			settings::set_setting("network_packet_loss_ppm", 0);
			settings::set_setting("network_very_late_ppm", 0);
			settings::set_setting("network_very_late_delay_ms", 1100);
			settings::set_setting("network_log_extra_delay_ms", 0);

			settings::ModuleSettings module_settings{};
			module_settings.active = true;
			settings::set_module_settings(module::ModuleId{MODULE}, module_settings);
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
		}

		static void run_for(sim::Runtime& runtime, std::chrono::milliseconds simulated) {
			const int frames = static_cast<int>(simulated.count() / 16) + 1;
			for (int frame = 0; frame < frames; frame++) {
				runtime.advance(std::chrono::microseconds(16000), 1.0, std::chrono::seconds(5));
			}
		}

		module::Module* hardware(sim::Runtime& runtime) { return runtime.world().module(module::ModuleId{MODULE}); }
	};

} // namespace

TEST_F(AppletTest, LaserAlignmentDrivesTheServoAndLedFromLaserState) {
	const std::string binary = applet_path("laser_alignment");
	if (binary.empty()) {
		GTEST_SKIP() << "No laser_alignment applet built; run (cd external/q_applets && ./q_sdk/qbuild sim)";
	}
	settings::set_setting<std::string>("application_path", binary);

	sim::Runtime runtime(1);
	ASSERT_NO_THROW(runtime.start());

	// The application enables both lasers, waits half a second, then polls them every 50 ms.
	run_for(runtime, std::chrono::milliseconds(2000));

	EXPECT_TRUE(hardware(runtime)->is_left_laser_enabled());
	EXPECT_TRUE(hardware(runtime)->is_right_laser_enabled());

	// Both beams clear means aligned: servo centred, LED green, as in the Go implementation.
	EXPECT_DOUBLE_EQ(hardware(runtime)->servo_angle().raw(), 90.0);
	EXPECT_EQ(hardware(runtime)->led_color().raw(), 0x003F00u);

	// Blocked with a tub, since the world derives laser state from tub position every tick.
	runtime.world().place(1, module::ModuleId{MODULE}, world::Position::LEFT_LASER, {});
	run_for(runtime, std::chrono::milliseconds(500));

	EXPECT_DOUBLE_EQ(hardware(runtime)->servo_angle().raw(), 0.0);
	EXPECT_EQ(hardware(runtime)->led_color().raw(), 0x00003Fu);

	runtime.stop();
}

TEST_F(AppletTest, ApplicationsActivateAndKeepTheirHeartbeatUp) {
	const std::string binary = applet_path("laser_alignment");
	if (binary.empty()) {
		GTEST_SKIP() << "No laser_alignment applet built; run (cd external/q_applets && ./q_sdk/qbuild sim)";
	}
	settings::set_setting<std::string>("application_path", binary);

	sim::Runtime runtime(1);
	ASSERT_NO_THROW(runtime.start());

	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{MODULE}));
	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::RUNNING);

	run_for(runtime, std::chrono::milliseconds(1000));

	// The application logs over RPC, so the simulation sees its diagnostics.
	EXPECT_FALSE(runtime.platform().logs(module::ModuleId{MODULE}).empty());
	EXPECT_TRUE(runtime.platform().panics(module::ModuleId{MODULE}).empty());

	runtime.stop();
}
