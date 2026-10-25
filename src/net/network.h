#ifndef NET_NETWORK_H
#define NET_NETWORK_H

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../module/types.h"

namespace net {

	// All delivery decisions derive from the master seed, so a run drops and delays the same
	// messages every time.
	// `upper` is a threshold out of one million: a draw below it takes `delay_ms`. The last entry's
	// `upper` must be 1,000,000.
	struct DelayBucket {
		uint64_t upper    = 0;
		uint64_t delay_ms = 0;
	};

	// Measured delay distribution for the physical_nominal_v0.1 profile.
	[[nodiscard]] std::vector<DelayBucket> default_delay_cdf();

	struct Config {
		// Part of every seeded decision's key, so different profiles never collide on the same seed.
		std::string profile = "physical_nominal_v0.1";

		// Cumulative thresholds out of one million. Leave empty to use fixed_delay_ms instead.
		std::vector<DelayBucket> delay_cdf      = default_delay_cdf();
		uint64_t                 fixed_delay_ms = 5;

		// Per-leg drop probability, out of one million. Zero disables loss.
		uint64_t packet_loss_ppm = 1700;

		// Per-leg probability, out of one million, of a very-late delay instead of the ordinary one.
		uint64_t very_late_ppm      = 15;
		uint64_t very_late_delay_ms = 1100;

		// Extra fixed delay added after loss and very-late are decided, for channels with an extra
		// hop such as logs and panic reports.
		uint64_t extra_delay_ms = 0;
	};

	struct Delivery {
		module::ModuleId     recipient;
		std::vector<uint8_t> payload;
	};

	class Model {
	private:
		struct Pending {
			uint64_t             due_tick = 0;
			uint64_t             ordinal  = 0;
			module::ModuleId     source;
			module::ModuleId     recipient;
			std::vector<uint8_t> payload;
		};

		uint64_t                    master_seed_ = 0;
		Config                      config_;
		std::vector<Pending>        pending_;
		std::map<uint8_t, uint64_t> next_ordinal_;
		uint64_t                    next_delivery_tick_ = UINT64_MAX;

		// Draws a value in [0, 1,000,000) for one keyed decision about one packet leg.
		[[nodiscard]] uint64_t draw_ppm(module::ModuleId source, module::ModuleId recipient, uint64_t ordinal,
										std::string_view decision) const;

		[[nodiscard]] bool is_dropped(module::ModuleId source, module::ModuleId recipient, uint64_t ordinal) const;

		[[nodiscard]] uint64_t pick_delay_ms(module::ModuleId source, module::ModuleId recipient,
											 uint64_t ordinal) const;

	public:
		Model() = default;

		Model(uint64_t master_seed, Config config) : master_seed_(master_seed), config_(config) {}

		// Each recipient is its own independent packet leg with its own ordinal, loss draw, and
		// delay draw. A broadcast includes the sender.
		void send(uint64_t tick, module::ModuleId sender, const std::vector<module::ModuleId>& recipients,
				  const std::vector<uint8_t>& payload);

		// Returns deliveries in canonical order (tick, source, destination, ordinal) rather than
		// host arrival order, which would not be reproducible.
		[[nodiscard]] std::vector<Delivery> deliver_due(uint64_t tick);

		[[nodiscard]] std::size_t in_flight() const noexcept { return pending_.size(); }
	};

	// The delivery model configured from settings.toml.
	[[nodiscard]] Config config_from_settings();

} // namespace net

#endif // NET_NETWORK_H
