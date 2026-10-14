#ifndef LTL_RPC_VOCABULARY_H
#define LTL_RPC_VOCABULARY_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <quercus/rpc_wire.h>

namespace ltl {
	inline constexpr int16_t RPC_STATUS_UNDELIVERED = INT16_MIN;

	inline const std::unordered_map<std::string, uint8_t>& rpc_commands() {
		static const std::unordered_map<std::string, uint8_t> values = {
			{"rpc_cmd_laser_left_detect", CMD_LASER_LEFT_DETECT},
			{"rpc_cmd_laser_left_get", CMD_LASER_LEFT_GET},
			{"rpc_cmd_laser_left_set", CMD_LASER_LEFT_SET},
			{"rpc_cmd_laser_right_detect", CMD_LASER_RIGHT_DETECT},
			{"rpc_cmd_laser_right_get", CMD_LASER_RIGHT_GET},
			{"rpc_cmd_laser_right_set", CMD_LASER_RIGHT_SET},
			{"rpc_cmd_belt_big_get_encoder_count", CMD_BELT_BIG_GET_ENCODER_COUNT},
			{"rpc_cmd_belt_big_get_encoder_freq", CMD_BELT_BIG_GET_ENCODER_FREQ},
			{"rpc_cmd_belt_big_get_speed", CMD_BELT_BIG_GET_SPEED},
			{"rpc_cmd_belt_big_set_speed", CMD_BELT_BIG_SET_SPEED},
			{"rpc_cmd_belt_small_get_encoder_count", CMD_BELT_SMALL_GET_ENCODER_COUNT},
			{"rpc_cmd_belt_small_get_encoder_freq", CMD_BELT_SMALL_GET_ENCODER_FREQ},
			{"rpc_cmd_belt_small_get_speed", CMD_BELT_SMALL_GET_SPEED},
			{"rpc_cmd_belt_small_set_speed", CMD_BELT_SMALL_SET_SPEED},
			{"rpc_cmd_servo_angle_get", CMD_SERVO_ANGLE_GET},
			{"rpc_cmd_servo_angle_set", CMD_SERVO_ANGLE_SET},
			{"rpc_cmd_led_get_color", CMD_LED_GET_COLOR},
			{"rpc_cmd_led_set_color", CMD_LED_SET_COLOR},
			{"rpc_cmd_rfid_check_tag", CMD_RFID_CHECK_TAG},
			{"rpc_cmd_rfid_get_uid", CMD_RFID_GET_UID},
			{"rpc_cmd_rfid_read_data_block", CMD_RFID_READ_DATA_BLOCK},
			{"rpc_cmd_rfid_write_data_block", CMD_RFID_WRITE_DATA_BLOCK},
			{"rpc_cmd_cv_sensor_get_current", CMD_CV_SENSOR_GET_CURRENT},
			{"rpc_cmd_cv_sensor_get_power", CMD_CV_SENSOR_GET_POWER},
			{"rpc_cmd_cv_sensor_get_voltage", CMD_CV_SENSOR_GET_VOLTAGE},
			{"rpc_cmd_send_packet", CMD_SEND_PACKET},
			{"rpc_cmd_get_own_id", CMD_GET_OWN_ID},
			{"rpc_cmd_get_network_map", CMD_GET_NETWORK_MAP},
			{"rpc_cmd_get_module_list", CMD_GET_MODULE_LIST},
			{"rpc_cmd_set_flight_schedule", CMD_SET_FLIGHT_SCHEDULE},
			{"rpc_cmd_broadcast_packet", CMD_BROADCAST_PACKET},
			{"rpc_cmd_subscribe_to_event", CMD_SUBSCRIBE_TO_EVENT},
			{"rpc_cmd_unsubscribe_from_event", CMD_UNSUBSCRIBE_FROM_EVENT},
			{"rpc_cmd_heartbeat", CMD_HEARTBEAT},
			{"rpc_cmd_log", CMD_LOG},
			{"rpc_cmd_panic_report", CMD_PANIC_REPORT}};
		return values;
	}

	inline const std::unordered_map<std::string, uint8_t>& rpc_event_kinds() {
		static const std::unordered_map<std::string, uint8_t> values = {
			{"rpc_event_none", EVENT_NONE},
			{"rpc_event_laser_left_detect", EVENT_LASER_LEFT_DETECT},
			{"rpc_event_laser_right_detect", EVENT_LASER_RIGHT_DETECT},
			{"rpc_event_rfid_detect", EVENT_RFID_DETECT},
			{"rpc_event_encoder_long_stopped", EVENT_ENCODER_LONG_STOPPED},
			{"rpc_event_encoder_short_stopped", EVENT_ENCODER_SHORT_STOPPED},
			{"rpc_event_current_threshold_reached", EVENT_CURRENT_THRESHOLD_REACHED},
			{"rpc_event_power_threshold_reached", EVENT_POWER_THRESHOLD_REACHED},
			{"rpc_event_message_received", EVENT_MESSAGE_RECEIVED}};
		return values;
	}

	inline const std::unordered_map<std::string, int16_t>& rpc_statuses() {
		static const std::unordered_map<std::string, int16_t> values = {{"undelivered", RPC_STATUS_UNDELIVERED},
																		{"ok", 0},
																		{"permission_denied", -1},
																		{"not_found", -2},
																		{"try_again", -11},
																		{"invalid", -22},
																		{"not_supported", -88}};
		return values;
	}

	template <typename T>
	inline std::optional<T> enum_value(const std::unordered_map<std::string, T>& values, const std::string& name) {
		const auto found = values.find(name);
		if (found == values.end()) return {};
		return found->second;
	}

} // namespace ltl

#endif
