#include "network.h"

#include <algorithm>
#include <tuple>

#include "../seed/seed.h"
#include "../settings.h"

namespace net {

	std::vector<DelayBucket> default_delay_cdf() {
		return {
			{30000, 5},   {130000, 8},  {380000, 9},   {680000, 10},  {730000, 11},  {810000, 13},   {890000, 15},
			{970000, 22}, {993000, 28}, {995000, 120}, {997000, 190}, {999000, 300}, {1000000, 400},
		};
	}

	Config config_from_settings() {
		Config config;
		config.packet_loss_ppm    = static_cast<uint64_t>(settings::get_setting<int>("network_packet_loss_ppm"));
		config.very_late_ppm      = static_cast<uint64_t>(settings::get_setting<int>("network_very_late_ppm"));
		config.very_late_delay_ms = static_cast<uint64_t>(settings::get_setting<int>("network_very_late_delay_ms"));

		// A negative override (the default) means use the profile's distribution; otherwise fixed.
		const int fixed_delay_ms = settings::get_setting<int>("network_fixed_delay_ms");
		if (fixed_delay_ms >= 0) {
			config.delay_cdf.clear();
			config.fixed_delay_ms = static_cast<uint64_t>(fixed_delay_ms);
		}

		return config;
	}

	uint64_t Model::draw_ppm(module::ModuleId source, module::ModuleId recipient, uint64_t ordinal,
							 std::string_view decision) const {
		const std::string label = config_.profile + "/" + std::to_string(source.raw()) + "/" +
								  std::to_string(recipient.raw()) + "/" + std::to_string(ordinal) + "/" +
								  std::string(decision);
		return seed::derive(master_seed_, label) % 1'000'000;
	}

	bool Model::is_dropped(module::ModuleId source, module::ModuleId recipient, uint64_t ordinal) const {
		if (config_.packet_loss_ppm == 0) {
			return false;
		}
		return draw_ppm(source, recipient, ordinal, "drop") < config_.packet_loss_ppm;
	}

	uint64_t Model::pick_delay_ms(module::ModuleId source, module::ModuleId recipient, uint64_t ordinal) const {
		if (config_.very_late_ppm != 0 && draw_ppm(source, recipient, ordinal, "late") < config_.very_late_ppm) {
			return config_.very_late_delay_ms;
		}

		if (config_.delay_cdf.empty()) {
			return config_.fixed_delay_ms;
		}

		const uint64_t roll = draw_ppm(source, recipient, ordinal, "delay");
		for (const DelayBucket& bucket : config_.delay_cdf) {
			if (roll < bucket.upper) {
				return bucket.delay_ms;
			}
		}
		// Rounding should never leave a roll uncovered, but fall back rather than return undefined.
		return config_.delay_cdf.back().delay_ms;
	}

	void Model::send(uint64_t tick, module::ModuleId sender, const std::vector<module::ModuleId>& recipients,
					 const std::vector<uint8_t>& payload) {
		for (const module::ModuleId recipient : recipients) {
			// Every leg is an independent packet; a dropped one is never enqueued.
			const uint64_t ordinal = next_ordinal_[sender.raw()]++;

			if (is_dropped(sender, recipient, ordinal)) {
				continue;
			}

			Pending message;
			message.due_tick  = tick + pick_delay_ms(sender, recipient, ordinal) + config_.extra_delay_ms;
			message.ordinal   = ordinal;
			message.source    = sender;
			message.recipient = recipient;
			message.payload   = payload;
			pending_.push_back(std::move(message));
		}
	}

	std::vector<Delivery> Model::deliver_due(uint64_t tick) {
		std::vector<Pending> due;
		std::vector<Pending> remaining;
		remaining.reserve(pending_.size());

		for (Pending& message : pending_) {
			if (message.due_tick <= tick) {
				due.push_back(std::move(message));
			} else {
				remaining.push_back(std::move(message));
			}
		}
		pending_ = std::move(remaining);

		if (due.empty()) {
			return {};
		}

		// Canonical order, so same-tick arrivals do not depend on storage order.
		std::sort(due.begin(), due.end(), [](const Pending& left, const Pending& right) {
			return std::make_tuple(left.due_tick, left.source.raw(), left.recipient.raw(), left.ordinal) <
				   std::make_tuple(right.due_tick, right.source.raw(), right.recipient.raw(), right.ordinal);
		});

		std::vector<Delivery> deliveries;
		deliveries.reserve(due.size());
		for (Pending& message : due) {
			deliveries.push_back(Delivery{message.recipient, std::move(message.payload)});
		}
		return deliveries;
	}

} // namespace net
