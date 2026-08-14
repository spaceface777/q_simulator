#include "observation.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include "rpc_vocabulary.h"

namespace ltl {

	namespace {

		constexpr double NUMERIC_TOLERANCE = 1e-6;

		struct ModuleRef {
			uint16_t id;
			bool     operator==(const ModuleRef&) const = default;
		};

		struct TubRef {
			uint16_t id;
			bool     operator==(const TubRef&) const = default;
		};

		struct PositionRef {
			uint16_t        module;
			world::Position position;
			bool            operator==(const PositionRef&) const = default;
		};

		struct EnumRef {
			ValueType type;
			int64_t   value;
			bool      operator==(const EnumRef&) const = default;
		};

		struct RecordRef {
			ValueType type;
			uint64_t  id;
			bool      operator==(const RecordRef&) const = default;
		};

		using EvaluatedValue = std::variant<bool, double, uint32_t, std::vector<uint8_t>, EnumRef, RecordRef, ModuleRef,
											TubRef, PositionRef>;
		using BindingMap     = std::unordered_map<std::string, uint64_t>;

		ModuleSnapshot snapshot(const module::Module& hardware) {
			return ModuleSnapshot{hardware.id().raw(),
								  hardware.big_belt_speed().raw(),
								  hardware.small_belt_speed().raw(),
								  hardware.servo_angle().raw(),
								  hardware.servo_actual_angle().raw(),
								  hardware.led_color().raw(),
								  hardware.is_left_laser_enabled(),
								  hardware.is_right_laser_enabled(),
								  hardware.is_left_laser_blocked(),
								  hardware.is_right_laser_blocked(),
								  hardware.is_rfid_active()};
		}

		std::map<uint16_t, ModuleSnapshot> snapshot_modules(const std::vector<module::ModuleId>& ids,
															const world::State&                  world) {
			std::map<uint16_t, ModuleSnapshot> result;
			for (const module::ModuleId id : ids) {
				const module::Module* hardware = world.module(id);
				if (hardware == nullptr) {
					throw std::logic_error("cannot observe module " + std::to_string(id.raw()) +
										   " because it is missing from the world");
				}
				result.emplace(id.raw(), snapshot(*hardware));
			}
			return result;
		}

		std::map<uint16_t, TubSnapshot> snapshot_tubs(const world::State& world) {
			std::map<uint16_t, TubSnapshot> result;
			for (const auto& [id, tub] : world.tubs()) {
				result.emplace(id,
							   TubSnapshot{id, tub.module.raw(), tub.belt, tub.position_um, world.position_of(tub)});
			}
			return result;
		}

		std::optional<world::Position> position_named(const std::string& name) {
			if (name == "entry") return world::Position::ENTRY;
			if (name == "center") return world::Position::CENTER;
			if (name == "left_laser") return world::Position::LEFT_LASER;
			if (name == "right_laser") return world::Position::RIGHT_LASER;
			return {};
		}

		std::optional<ModuleField> field_named(const std::string& name) {
			if (name == "big_belt_speed") return ModuleField::BIG_BELT_SPEED;
			if (name == "small_belt_speed") return ModuleField::SMALL_BELT_SPEED;
			if (name == "servo_angle") return ModuleField::SERVO_ANGLE;
			if (name == "servo_physical_angle") return ModuleField::SERVO_PHYSICAL_ANGLE;
			if (name == "led_color") return ModuleField::LED_COLOR;
			if (name == "left_laser_enabled") return ModuleField::LEFT_LASER_ENABLED;
			if (name == "right_laser_enabled") return ModuleField::RIGHT_LASER_ENABLED;
			if (name == "left_laser_blocked") return ModuleField::LEFT_LASER_BLOCKED;
			if (name == "right_laser_blocked") return ModuleField::RIGHT_LASER_BLOCKED;
			if (name == "rfid_active") return ModuleField::RFID_ACTIVE;
			return {};
		}

		const ModuleSnapshot& module_snapshot(const Observation& observation, uint16_t id) {
			const auto found = observation.modules().find(id);
			if (found == observation.modules().end()) {
				throw std::logic_error("typed LTL atom refers to unobserved module " + std::to_string(id));
			}
			return found->second;
		}

		EvaluatedValue evaluate_value(const Value& value, const Observation& observation, BindingMap& bindings);
		bool           evaluate_formula(const Formula& formula, const Observation& observation, BindingMap& bindings);
		bool           equal(const EvaluatedValue& left, const EvaluatedValue& right, ValueType type);

		ModuleRef evaluate_module(const Value& value, const Observation& observation, BindingMap& bindings) {
			const EvaluatedValue evaluated = evaluate_value(value, observation, bindings);
			if (!std::holds_alternative<ModuleRef>(evaluated)) throw std::logic_error("expected typed Module value");
			return std::get<ModuleRef>(evaluated);
		}

		TubRef evaluate_tub(const Value& value, const Observation& observation, BindingMap& bindings) {
			const EvaluatedValue evaluated = evaluate_value(value, observation, bindings);
			if (!std::holds_alternative<TubRef>(evaluated)) throw std::logic_error("expected typed Tub value");
			return std::get<TubRef>(evaluated);
		}

		PositionRef evaluate_position(const Value& value, const Observation& observation, BindingMap& bindings) {
			const EvaluatedValue evaluated = evaluate_value(value, observation, bindings);
			if (!std::holds_alternative<PositionRef>(evaluated))
				throw std::logic_error("expected typed Position value");
			return std::get<PositionRef>(evaluated);
		}

		bool evaluate_call(const Value& value, const Observation& observation, BindingMap& bindings) {
			if (value.text == "present") {
				return observation.tubs().contains(evaluate_tub(*value.arguments[0], observation, bindings).id);
			}
			if (value.text == "at_module") {
				const TubRef    tub    = evaluate_tub(*value.arguments[0], observation, bindings);
				const ModuleRef module = evaluate_module(*value.arguments[1], observation, bindings);
				const auto      found  = observation.tubs().find(tub.id);
				return found != observation.tubs().end() && found->second.module == module.id;
			}
			if (value.text == "at_position") {
				const TubRef      tub      = evaluate_tub(*value.arguments[0], observation, bindings);
				const PositionRef position = evaluate_position(*value.arguments[1], observation, bindings);
				const auto        found    = observation.tubs().find(tub.id);
				return found != observation.tubs().end() && found->second.module == position.module &&
					   found->second.position == position.position;
			}
			if (value.text == "appeared") {
				return observation.appeared().contains(evaluate_tub(*value.arguments[0], observation, bindings).id);
			}
			if (value.text == "disappeared") {
				return observation.disappeared().contains(evaluate_tub(*value.arguments[0], observation, bindings).id);
			}
			if (value.text == "entered" || value.text == "left") {
				const TubRef      tub      = evaluate_tub(*value.arguments[0], observation, bindings);
				const PositionRef position = evaluate_position(*value.arguments[1], observation, bindings);
				const TubPosition edge{tub.id, position.module, position.position};
				return value.text == "entered" ? observation.entered().contains(edge)
											   : observation.left().contains(edge);
			}
			if (value.text == "changed") {
				const Value&                     member = *value.arguments[0];
				const ModuleRef                  module = evaluate_module(*member.base, observation, bindings);
				const std::optional<ModuleField> field  = field_named(member.text);
				if (!field) throw std::logic_error("typed changed atom contains an unknown field");
				return observation.changed().contains(ChangedField{module.id, *field});
			}
			throw std::logic_error("typed LTL atom contains an unknown call");
		}

		uint64_t evaluate_count(const Value& value, const Observation& observation, BindingMap& bindings) {
			uint64_t result = 0;
			observation.visit_domain(value.domain.get(), value.binding_type, [&](uint64_t identity) {
				bindings[value.binding] = identity;
				if (evaluate_formula(*value.predicate, observation, bindings)) result++;
			});
			bindings.erase(value.binding);
			return result;
		}

		EvaluatedValue evaluate_value(const Value& value, const Observation& observation, BindingMap& bindings) {
			switch (value.kind) {
			case Value::Kind::BOOLEAN: return value.boolean;
			case Value::Kind::NUMBER:  return value.number;
			case Value::Kind::COLOR:   return value.color;
			case Value::Kind::BYTES:   return value.bytes;
			case Value::Kind::ENUM_LITERAL:
				if (value.type == ValueType::RPC_COMMAND)
					return EnumRef{value.type, *enum_value(rpc_commands(), value.text)};
				if (value.type == ValueType::RPC_STATUS)
					return EnumRef{value.type, *enum_value(rpc_statuses(), value.text)};
				return EnumRef{value.type, *enum_value(rpc_event_kinds(), value.text)};
			case Value::Kind::COUNT: return static_cast<double>(evaluate_count(value, observation, bindings));
			case Value::Kind::NAME:
				if (value.type == ValueType::RPC_REQUEST || value.type == ValueType::RPC_RESPONSE ||
					value.type == ValueType::RPC_EVENT) {
					const auto     found = bindings.find(value.text);
					const uint64_t id =
						value.entity_id != 0 ? value.entity_id : (found == bindings.end() ? 0 : found->second);
					if (id == 0) throw std::logic_error("unresolved typed LTL binding '" + value.text + "'");
					return RecordRef{value.type, id};
				}
				if (value.type == ValueType::MODULE || value.type == ValueType::TUB) {
					const auto     found = bindings.find(value.text);
					const uint16_t id    = value.entity_id != 0
											   ? value.entity_id
											   : static_cast<uint16_t>(found == bindings.end() ? 0 : found->second);
					if (id == 0) throw std::logic_error("unresolved typed LTL binding '" + value.text + "'");
					if (value.type == ValueType::MODULE) return ModuleRef{id};
					return TubRef{id};
				}
				break;
			case Value::Kind::CALL:
				if (value.type == ValueType::RPC_RESPONSE) {
					const RecordRef request =
						std::get<RecordRef>(evaluate_value(*value.arguments[0], observation, bindings));
					return RecordRef{ValueType::RPC_RESPONSE, request.id};
				}
				if (value.type == ValueType::MODULE) return ModuleRef{static_cast<uint16_t>(value.entity_id)};
				if (value.type == ValueType::TUB) return TubRef{static_cast<uint16_t>(value.entity_id)};
				if (value.type == ValueType::BOOLEAN) return evaluate_call(value, observation, bindings);
				break;
			case Value::Kind::MEMBER: {
				if (value.base->type == ValueType::RPC_REQUEST || value.base->type == ValueType::RPC_RESPONSE ||
					value.base->type == ValueType::RPC_EVENT) {
					const RecordRef record = std::get<RecordRef>(evaluate_value(*value.base, observation, bindings));
					if (record.type == ValueType::RPC_REQUEST) {
						const RpcRequest* request = observation.rpc_request(record.id);
						if (!request) throw std::logic_error("unknown frozen RPC request");
						if (value.text == "id") return static_cast<double>(request->id);
						if (value.text == "call_id") return static_cast<double>(request->call_id);
						if (value.text == "module") return ModuleRef{request->module};
						if (value.text == "cmd") return EnumRef{ValueType::RPC_COMMAND, request->cmd};
						if (value.text == "payload") return request->payload;
						if (value.text == "confirmable") return request->confirmable;
					}
					if (record.type == ValueType::RPC_RESPONSE) {
						const RpcRequest*  request  = observation.rpc_request(record.id);
						const RpcResponse* response = observation.rpc_response_to(record.id);
						if (!request) throw std::logic_error("unknown correlated RPC request");
						if (value.text == "id") return static_cast<double>(record.id);
						if (value.text == "request") return RecordRef{ValueType::RPC_REQUEST, record.id};
						if (value.text == "module") return ModuleRef{request->module};
						if (value.text == "cmd") return EnumRef{ValueType::RPC_COMMAND, request->cmd};
						if (value.text == "status")
							return EnumRef{ValueType::RPC_STATUS, response ? response->status : RPC_STATUS_UNDELIVERED};
						if (value.text == "payload") return response ? response->payload : std::vector<uint8_t>{};
					}
					if (record.type == ValueType::RPC_EVENT) {
						const RpcEvent* event = observation.rpc_event(record.id);
						if (!event) throw std::logic_error("unknown RPC event");
						if (value.text == "id") return static_cast<double>(event->id);
						if (value.text == "module") return ModuleRef{event->module};
						if (value.text == "kind") return EnumRef{ValueType::RPC_EVENT_KIND, event->kind};
						if (value.text == "payload") return event->payload;
					}
					break;
				}
				const ModuleRef module = evaluate_module(*value.base, observation, bindings);
				if (const std::optional<world::Position> position = position_named(value.text)) {
					return PositionRef{module.id, *position};
				}
				const ModuleSnapshot& state = module_snapshot(observation, module.id);
				if (value.text == "big_belt_speed") return state.big_belt_speed;
				if (value.text == "small_belt_speed") return state.small_belt_speed;
				if (value.text == "servo_angle") return state.servo_angle;
				if (value.text == "servo_physical_angle") return state.servo_physical_angle;
				if (value.text == "led_color") return state.led_color;
				if (value.text == "left_laser_enabled") return state.left_laser_enabled;
				if (value.text == "right_laser_enabled") return state.right_laser_enabled;
				if (value.text == "left_laser_blocked") return state.left_laser_blocked;
				if (value.text == "right_laser_blocked") return state.right_laser_blocked;
				if (value.text == "rfid_active") return state.rfid_active;
				break;
			}
			}
			throw std::logic_error("cannot evaluate typed LTL value");
		}

		bool evaluate_formula(const Formula& formula, const Observation& observation, BindingMap& bindings) {
			if (formula.kind == Formula::Kind::ATOM) {
				return std::get<bool>(evaluate_value(*formula.value, observation, bindings));
			}
			if (formula.kind == Formula::Kind::COMPARISON) {
				const EvaluatedValue left  = evaluate_value(*formula.value, observation, bindings);
				const EvaluatedValue right = evaluate_value(*formula.compared, observation, bindings);
				switch (formula.comparison) {
				case Formula::Comparison::EQUAL:         return equal(left, right, formula.value->type);
				case Formula::Comparison::NOT_EQUAL:     return !equal(left, right, formula.value->type);
				case Formula::Comparison::LESS:          return std::get<double>(left) < std::get<double>(right);
				case Formula::Comparison::LESS_EQUAL:    return std::get<double>(left) <= std::get<double>(right);
				case Formula::Comparison::GREATER:       return std::get<double>(left) > std::get<double>(right);
				case Formula::Comparison::GREATER_EQUAL: return std::get<double>(left) >= std::get<double>(right);
				}
			}
			if (formula.kind == Formula::Kind::NOT) return !evaluate_formula(*formula.left, observation, bindings);
			if (formula.kind == Formula::Kind::AND)
				return evaluate_formula(*formula.left, observation, bindings) &&
					   evaluate_formula(*formula.right, observation, bindings);
			if (formula.kind == Formula::Kind::OR)
				return evaluate_formula(*formula.left, observation, bindings) ||
					   evaluate_formula(*formula.right, observation, bindings);
			if (formula.kind == Formula::Kind::IMPLIES)
				return !evaluate_formula(*formula.left, observation, bindings) ||
					   evaluate_formula(*formula.right, observation, bindings);
			throw std::logic_error("count predicate contains a non-current formula");
		}

		bool equal(const EvaluatedValue& left, const EvaluatedValue& right, ValueType type) {
			if (type == ValueType::NUMBER) {
				return std::fabs(std::get<double>(left) - std::get<double>(right)) <= NUMERIC_TOLERANCE;
			}
			return left == right;
		}

		std::string render_value(const EvaluatedValue& value, ValueType type) {
			std::ostringstream output;
			if (const auto boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
			if (const auto number = std::get_if<double>(&value)) {
				output << std::setprecision(17) << *number;
				return output.str();
			}
			if (const auto color = std::get_if<uint32_t>(&value)) {
				if (type == ValueType::COLOR) {
					output << "0x" << std::hex << std::setw(6) << std::setfill('0') << *color;
				} else {
					output << *color;
				}
				return output.str();
			}
			if (const auto bytes = std::get_if<std::vector<uint8_t>>(&value)) {
				output << '"';
				for (const uint8_t byte : *bytes) {
					if (byte == '"') output << "\\\"";
					else if (byte == '\\') output << "\\\\";
					else if (byte == '\n') output << "\\n";
					else if (byte == '\r') output << "\\r";
					else if (byte == '\t') output << "\\t";
					else if (byte >= 0x20 && byte <= 0x7e) output << static_cast<char>(byte);
					else
						output << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte)
							   << std::dec;
				}
				output << '"';
				return output.str();
			}
			if (const auto enumeration = std::get_if<EnumRef>(&value)) {
				const auto render = [&](const auto& values) {
					for (const auto& [name, raw] : values)
						if (raw == enumeration->value) return "." + name;
					return std::string("<unknown>");
				};
				if (enumeration->type == ValueType::RPC_COMMAND) return render(rpc_commands());
				if (enumeration->type == ValueType::RPC_STATUS) return render(rpc_statuses());
				return render(rpc_event_kinds());
			}
			if (const auto module = std::get_if<ModuleRef>(&value)) return "module(" + std::to_string(module->id) + ")";
			if (const auto tub = std::get_if<TubRef>(&value)) return "tub(" + std::to_string(tub->id) + ")";
			if (const auto position = std::get_if<PositionRef>(&value)) {
				static constexpr const char* names[] = {"entry", "center", "left_laser", "right_laser"};
				return "module(" + std::to_string(position->module) + ")." +
					   names[static_cast<unsigned>(position->position)];
			}
			throw std::logic_error("cannot render typed LTL value");
		}

		bool different(ModuleField field, const ModuleSnapshot& left, const ModuleSnapshot& right) {
			switch (field) {
			case ModuleField::BIG_BELT_SPEED:       return left.big_belt_speed != right.big_belt_speed;
			case ModuleField::SMALL_BELT_SPEED:     return left.small_belt_speed != right.small_belt_speed;
			case ModuleField::SERVO_ANGLE:          return left.servo_angle != right.servo_angle;
			case ModuleField::SERVO_PHYSICAL_ANGLE: return left.servo_physical_angle != right.servo_physical_angle;
			case ModuleField::LED_COLOR:            return left.led_color != right.led_color;
			case ModuleField::LEFT_LASER_ENABLED:   return left.left_laser_enabled != right.left_laser_enabled;
			case ModuleField::RIGHT_LASER_ENABLED:  return left.right_laser_enabled != right.right_laser_enabled;
			case ModuleField::LEFT_LASER_BLOCKED:   return left.left_laser_blocked != right.left_laser_blocked;
			case ModuleField::RIGHT_LASER_BLOCKED:  return left.right_laser_blocked != right.right_laser_blocked;
			case ModuleField::RFID_ACTIVE:          return left.rfid_active != right.rfid_active;
			}
			return false;
		}

		void derive_tub_edges(const std::map<uint16_t, TubSnapshot>& current_tubs,
							  const std::map<uint16_t, TubSnapshot>& previous, std::set<uint16_t>& appeared,
							  std::set<uint16_t>& disappeared, std::set<TubPosition>& entered,
							  std::set<TubPosition>& left) {
			for (const auto& [id, current] : current_tubs) {
				const auto before = previous.find(id);
				if (before == previous.end()) appeared.insert(id);

				if (current.position && (before == previous.end() || before->second.module != current.module ||
										 before->second.position != current.position)) {
					entered.insert(TubPosition{id, current.module, *current.position});
				}
				if (before != previous.end() && before->second.position &&
					(before->second.module != current.module || before->second.position != current.position)) {
					left.insert(TubPosition{id, before->second.module, *before->second.position});
				}
			}

			for (const auto& [id, before] : previous) {
				if (current_tubs.contains(id)) continue;
				disappeared.insert(id);
				if (before.position) left.insert(TubPosition{id, before.module, *before.position});
			}
		}

		void derive_module_edges(const std::map<uint16_t, ModuleSnapshot>& current_modules,
								 const std::map<uint16_t, ModuleSnapshot>& previous, std::set<ChangedField>& changed) {
			for (const auto& [id, current] : current_modules) {
				const auto before = previous.find(id);
				if (before == previous.end()) throw std::logic_error("module appeared after observer construction");
				for (ModuleField field :
					 {ModuleField::BIG_BELT_SPEED, ModuleField::SMALL_BELT_SPEED, ModuleField::SERVO_ANGLE,
					  ModuleField::SERVO_PHYSICAL_ANGLE, ModuleField::LED_COLOR, ModuleField::LEFT_LASER_ENABLED,
					  ModuleField::RIGHT_LASER_ENABLED, ModuleField::LEFT_LASER_BLOCKED,
					  ModuleField::RIGHT_LASER_BLOCKED, ModuleField::RFID_ACTIVE}) {
					if (different(field, before->second, current)) {
						changed.insert(ChangedField{id, field});
					}
				}
			}
		}

	} // namespace

	Observer::Observer(const std::vector<module::ModuleId>& modules, const world::State& baseline)
		: module_ids_(modules), previous_modules_(snapshot_modules(modules, baseline)) {}

	Observation Observer::observe(uint64_t logical_tick, const world::State& world, std::vector<RpcRequest> requests,
								  std::vector<RpcResponse> responses, std::vector<RpcEvent> events,
								  std::map<uint16_t, bool> module_stepped) {
		Observation observation;
		observation.tick_           = logical_tick;
		observation.timestamp_      = std::chrono::milliseconds(logical_tick);
		observation.module_stepped_ = std::move(module_stepped);
		observation.modules_        = snapshot_modules(module_ids_, world);
		observation.tubs_           = snapshot_tubs(world);
		for (const auto& [id, tub] : observation.tubs_) {
			(void)tub;
			if (!previous_tubs_.contains(id) && seen_tubs_.contains(id)) {
				throw std::logic_error("tub " + std::to_string(id) + " was reintroduced after removal");
			}
			seen_tubs_.insert(id);
		}
		observation.known_tubs_    = seen_tubs_;
		observation.rpc_requests_  = std::move(requests);
		observation.rpc_responses_ = std::move(responses);
		observation.rpc_events_    = std::move(events);
		observation.observer_      = this;
		for (const RpcRequest& request : observation.rpc_requests_) {
			if (!rpc_request_registry_.emplace(request.id, request).second) {
				throw std::logic_error("duplicate RPC request monitor identity");
			}
		}
		for (const RpcResponse& response : observation.rpc_responses_) {
			const auto request = rpc_request_registry_.find(response.request_id);
			if (request == rpc_request_registry_.end()) {
				throw std::logic_error("RPC response refers to unknown request identity " +
									   std::to_string(response.request_id));
			}
			if (response.id != response.request_id || response.module != request->second.module ||
				response.cmd != request->second.cmd || response.call_id != request->second.call_id) {
				throw std::logic_error("RPC response correlation metadata does not match request identity " +
									   std::to_string(response.request_id));
			}
			if (!rpc_response_registry_.emplace(response.request_id, response).second) {
				throw std::logic_error("duplicate RPC response identity " + std::to_string(response.request_id));
			}
		}
		for (const RpcEvent& event : observation.rpc_events_) {
			if (!rpc_event_registry_.emplace(event.id, event).second) {
				throw std::logic_error("duplicate RPC event identity " + std::to_string(event.id));
			}
		}
		derive_tub_edges(observation.tubs_, previous_tubs_, observation.appeared_, observation.disappeared_,
						 observation.entered_, observation.left_);
		derive_module_edges(observation.modules_, previous_modules_, observation.changed_);
		previous_tubs_    = observation.tubs_;
		previous_modules_ = observation.modules_;
		return observation;
	}

	bool Observation::evaluate(const Formula& atom) const {
		BindingMap bindings;
		return evaluate_formula(atom, *this, bindings);
	}

	std::optional<std::string> Observation::actual_value(const Formula& atom) const {
		if (atom.kind != Formula::Kind::COMPARISON) return std::nullopt;
		BindingMap  bindings;
		std::string actual = render_value(evaluate_value(*atom.value, *this, bindings), atom.value->type);

		const Value* record = atom.value.get();
		while (record && record->kind == Value::Kind::MEMBER) record = record->base.get();
		if (record && record->kind == Value::Kind::CALL && record->type == ValueType::RPC_RESPONSE) {
			record = record->arguments[0].get();
		}
		if (record && record->kind == Value::Kind::NAME && record->entity_id != 0) {
			if (record->type == ValueType::RPC_REQUEST || record->type == ValueType::RPC_RESPONSE) {
				if (const RpcRequest* request = rpc_request(record->entity_id)) {
					actual += " [request.id=" + std::to_string(request->id) + ", module=module(" +
							  std::to_string(request->module) + "), cmd=" +
							  render_value(EnumRef{ValueType::RPC_COMMAND, request->cmd}, ValueType::RPC_COMMAND) +
							  ", call_id=" + std::to_string(request->call_id) + "]";
				}
			} else if (record->type == ValueType::RPC_EVENT) {
				if (const RpcEvent* event = rpc_event(record->entity_id)) {
					actual += " [event.id=" + std::to_string(event->id) + ", module=module(" +
							  std::to_string(event->module) + "), kind=" +
							  render_value(EnumRef{ValueType::RPC_EVENT_KIND, event->kind}, ValueType::RPC_EVENT_KIND) +
							  "]";
				}
			}
		}
		return actual;
	}

	void Observation::visit_domain(const Value* expression, ValueType type,
								   const std::function<void(uint64_t)>& visit) const {
		if (expression) {
			BindingMap      bindings;
			const ModuleRef module = evaluate_module(*expression->arguments[0], *this, bindings);
			if (type == ValueType::RPC_REQUEST) {
				for (const RpcRequest& request : rpc_requests_)
					if (request.module == module.id) visit(request.id);
				return;
			}
			if (type == ValueType::RPC_RESPONSE) {
				for (const RpcResponse& response : rpc_responses_)
					if (response.module == module.id) visit(response.request_id);
				return;
			}
			if (type == ValueType::RPC_EVENT) {
				for (const RpcEvent& event : rpc_events_)
					if (event.module == module.id) visit(event.id);
				return;
			}
			throw std::logic_error("unsupported LTL expression domain");
		}
		if (type == ValueType::MODULE) {
			for (const auto& [id, module] : modules_) {
				(void)module;
				visit(id);
			}
			return;
		}
		if (type == ValueType::TUB) {
			for (const uint16_t id : known_tubs_) visit(id);
			return;
		}
		throw std::logic_error("unsupported persistent LTL domain");
	}

	const RpcRequest* Observer::rpc_request(uint64_t id) const {
		const auto found = rpc_request_registry_.find(id);
		return found == rpc_request_registry_.end() ? nullptr : &found->second;
	}

	const RpcResponse* Observer::rpc_response_to(uint64_t request_id, uint64_t observation_tick) const {
		const auto found = rpc_response_registry_.find(request_id);
		return found == rpc_response_registry_.end() || found->second.tick > observation_tick ? nullptr
																							  : &found->second;
	}

	const RpcEvent* Observer::rpc_event(uint64_t id, uint64_t observation_tick) const {
		const auto found = rpc_event_registry_.find(id);
		return found == rpc_event_registry_.end() || found->second.tick > observation_tick ? nullptr : &found->second;
	}

	const RpcRequest* Observation::rpc_request(uint64_t id) const {
		return observer_ ? observer_->rpc_request(id) : nullptr;
	}

	const RpcResponse* Observation::rpc_response_to(uint64_t request_id) const {
		return observer_ ? observer_->rpc_response_to(request_id, tick_) : nullptr;
	}

	const RpcEvent* Observation::rpc_event(uint64_t id) const {
		return observer_ ? observer_->rpc_event(id, tick_) : nullptr;
	}

} // namespace ltl
