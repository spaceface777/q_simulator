#include "observation.h"
#include <atomic>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <span>
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

		struct Numeric {
			bool    integral = true;
			int64_t integer  = 0;
			double  physical = 0.0;
		};

		struct RecordRef {
			ValueType type;
			uint64_t  id;
			bool      operator==(const RecordRef&) const = default;
		};

		using BytesRef = std::span<const uint8_t>;
		using EvaluatedValue =
			std::variant<bool, Numeric, uint32_t, BytesRef, EnumRef, RecordRef, ModuleRef, TubRef, PositionRef>;

		// Quantifier bindings in scope. There are only ever a few, so a flat list beats hashing.
		class BindingMap {
		public:
			using Entry = std::pair<std::string_view, uint64_t>;

			const Entry* find(std::string_view name) const {
				for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry)
					if (entry->first == name) return &*entry;
				return nullptr;
			}

			static const Entry* end() { return nullptr; }

			uint64_t& operator[](std::string_view name) {
				for (Entry& entry : entries_)
					if (entry.first == name) return entry.second;
				return entries_.emplace_back(name, 0).second;
			}

			void erase(std::string_view name) {
				std::erase_if(entries_, [name](const Entry& entry) { return entry.first == name; });
			}

		private:
			std::vector<Entry> entries_;
		};

		ModuleSnapshot snapshot(const module::Module& hardware) {
			const auto& calibration = world::calibration_for(hardware.id());
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
								  hardware.is_rfid_active(),
								  calibration.endpoint_to_junction_um,
								  world::tub_radius_um(calibration)};
		}

		using ModuleSnapshots = std::map<uint16_t, ModuleSnapshot>;

		std::shared_ptr<const ModuleSnapshots> snapshot_modules(const std::vector<module::ModuleId>&   ids,
																const world::State&                    world,
																std::shared_ptr<const ModuleSnapshots> previous = {},
																const module::Changes* changes = nullptr) {
			std::shared_ptr<ModuleSnapshots> result;
			if (!previous) result = std::make_shared<ModuleSnapshots>();
			for (const module::ModuleId id : ids) {
				if (changes && !(*changes)[id.raw()]) {
					const auto& before      = previous->at(id.raw());
					const auto& calibration = world::calibration_for(id);
					if (before.leg_length_um == calibration.endpoint_to_junction_um &&
						before.tub_radius_um == world::tub_radius_um(calibration))
						continue;
				}
				const module::Module* hardware = world.module(id);
				if (hardware == nullptr) {
					throw std::logic_error("cannot observe module " + std::to_string(id.raw()) +
										   " because it is missing from the world");
				}
				const ModuleSnapshot current = snapshot(*hardware);
				if (previous && previous->at(id.raw()) == current) continue;
				if (!result) result = std::make_shared<ModuleSnapshots>(*previous);
				(*result)[id.raw()] = current;
			}
			return result ? result : previous;
		}

		using TubSnapshots = std::map<uint16_t, TubSnapshot>;

		// Whether a world tub has exactly this snapshot, given the previous snapshot of the same tub.
		bool same_tub(const world::State& world, const world::Tub& tub, const TubSnapshot& snapshot) {
			if (snapshot.module != tub.module.raw() || snapshot.previous_module != 0 || snapshot.belt != tub.belt ||
				snapshot.position_um != tub.position_um || snapshot.position != world.position_of(tub) ||
				!std::equal(snapshot.uid.begin(), snapshot.uid.end(), tub.uid.begin(), tub.uid.end()))
				return false;
			auto data = snapshot.data.begin();
			for (const RfidPage& page : tub.payload) {
				if (static_cast<std::size_t>(snapshot.data.end() - data) < page.size() ||
					!std::equal(page.begin(), page.end(), data))
					return false;
				data += static_cast<std::ptrdiff_t>(page.size());
			}
			return data == snapshot.data.end();
		}

		// Snapshots every tub. Returns the previous snapshots when no tub changed. A tub whose module
		// changed since the previous snapshots records that module as its previous module.
		std::shared_ptr<const TubSnapshots> snapshot_tubs(const world::State&                        world,
														  const std::shared_ptr<const TubSnapshots>& previous) {
			if (previous && previous->size() == world.tubs().size() &&
				std::equal(world.tubs().begin(), world.tubs().end(), previous->begin(),
						   [&](const auto& tub, const auto& old) {
							   return tub.first == old.first && same_tub(world, tub.second, old.second);
						   }))
				return previous;

			auto result = std::make_shared<TubSnapshots>();
			for (const auto& [id, tub] : world.tubs()) {
				TubSnapshot value{id, tub.module.raw(), tub.belt, tub.position_um, world.position_of(tub), {}, {}};
				value.uid.assign(tub.uid.begin(), tub.uid.end());
				value.data.reserve(RFID_PAYLOAD_PAGE_COUNT * RFID_PAGE_SIZE_BYTES);
				for (const RfidPage& page : tub.payload) value.data.insert(value.data.end(), page.begin(), page.end());
				if (previous) {
					const auto before = previous->find(id);
					if (before != previous->end() && before->second.module != value.module)
						value.previous_module = before->second.module;
				}
				result->emplace(id, std::move(value));
			}
			return result;
		}

		ModuleField module_field(Value::Symbol symbol) {
			switch (symbol) {
			case Value::Symbol::BIG_BELT_SPEED:       return ModuleField::BIG_BELT_SPEED;
			case Value::Symbol::SMALL_BELT_SPEED:     return ModuleField::SMALL_BELT_SPEED;
			case Value::Symbol::SERVO_ANGLE:          return ModuleField::SERVO_ANGLE;
			case Value::Symbol::SERVO_PHYSICAL_ANGLE: return ModuleField::SERVO_PHYSICAL_ANGLE;
			case Value::Symbol::LED_COLOR:            return ModuleField::LED_COLOR;
			case Value::Symbol::LED_VALUE:            return ModuleField::LED_COLOR;
			case Value::Symbol::LEFT_LASER_ENABLED:   return ModuleField::LEFT_LASER_ENABLED;
			case Value::Symbol::RIGHT_LASER_ENABLED:  return ModuleField::RIGHT_LASER_ENABLED;
			case Value::Symbol::LEFT_LASER_BLOCKED:   return ModuleField::LEFT_LASER_BLOCKED;
			case Value::Symbol::RIGHT_LASER_BLOCKED:  return ModuleField::RIGHT_LASER_BLOCKED;
			case Value::Symbol::RFID_ACTIVE:          return ModuleField::RFID_ACTIVE;
			default:                                  throw std::logic_error("typed changed atom contains an unknown field");
			}
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

		using channel::TubField;

		bool evaluate_call(const Value& value, const Observation& observation, BindingMap& bindings) {
			if (value.symbol == Value::Symbol::PRESENT) {
				const TubRef tub = evaluate_tub(*value.arguments[0], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::PRESENT));
				return observation.tubs().contains(tub.id);
			}
			if (value.symbol == Value::Symbol::SEEN) {
				// The tub has appeared at or before this observation. Guards initial_data and initial_uid.
				const TubRef tub = evaluate_tub(*value.arguments[0], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::INITIAL));
				return observation.initial_tub(tub.id) != nullptr;
			}
			if (value.symbol == Value::Symbol::AT_MODULE) {
				const TubRef    tub    = evaluate_tub(*value.arguments[0], observation, bindings);
				const ModuleRef module = evaluate_module(*value.arguments[1], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::PRESENT));
				observation.note(channel::tub(tub.id, TubField::MODULE));
				const auto found = observation.tubs().find(tub.id);
				return found != observation.tubs().end() && found->second.module == module.id;
			}
			if (value.symbol == Value::Symbol::AT_POSITION) {
				const TubRef      tub      = evaluate_tub(*value.arguments[0], observation, bindings);
				const PositionRef position = evaluate_position(*value.arguments[1], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::PRESENT));
				observation.note(channel::tub(tub.id, TubField::MODULE));
				observation.note(channel::tub(tub.id, TubField::POSITION));
				const auto found = observation.tubs().find(tub.id);
				return found != observation.tubs().end() && found->second.module == position.module &&
					   found->second.position == position.position;
			}
			if (value.symbol == Value::Symbol::APPEARED) {
				const TubRef tub = evaluate_tub(*value.arguments[0], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::APPEARED));
				return observation.appeared().contains(tub.id);
			}
			if (value.symbol == Value::Symbol::DISAPPEARED) {
				const TubRef tub = evaluate_tub(*value.arguments[0], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::DISAPPEARED));
				return observation.disappeared().contains(tub.id);
			}
			if (value.symbol == Value::Symbol::PHYSICAL_EXIT) {
				const TubRef tub = evaluate_tub(*value.arguments[0], observation, bindings);
				observation.note(channel::tub(tub.id, TubField::EXIT));
				const TubExit* exit = observation.tub_exit(tub.id);
				return exit && exit->cause == world::TubRemovalReason::YEETED;
			}
			if (value.symbol == Value::Symbol::ENTERED || value.symbol == Value::Symbol::LEFT) {
				const TubRef      tub      = evaluate_tub(*value.arguments[0], observation, bindings);
				const PositionRef position = evaluate_position(*value.arguments[1], observation, bindings);
				const TubPosition edge{tub.id, position.module, position.position};
				observation.note(channel::tub(tub.id, TubField::EDGES));
				return value.symbol == Value::Symbol::ENTERED ? observation.entered().contains(edge)
															  : observation.left().contains(edge);
			}
			if (value.symbol == Value::Symbol::CHANGED) {
				const Value&    member = *value.arguments[0];
				const ModuleRef module = evaluate_module(*member.base, observation, bindings);
				observation.note(channel::module_edge(module.id, module_field(member.symbol)));
				return observation.changed().contains(ChangedField{module.id, module_field(member.symbol)});
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

		Numeric numeric(const EvaluatedValue& value) {
			if (!std::holds_alternative<Numeric>(value)) throw std::logic_error("expected numeric LTL value");
			return std::get<Numeric>(value);
		}

		int64_t integer(const EvaluatedValue& value, const Span&) {
			const Numeric number = numeric(value);
			if (!number.integral) throw std::runtime_error("integer operation received a fractional value");
			return number.integer;
		}

		Numeric integer_value(uint64_t value) {
			return Numeric{true, static_cast<int64_t>(value), static_cast<double>(static_cast<int64_t>(value))};
		}

		Numeric physical_value(double value) {
			return Numeric{false, 0, value};
		}

		double as_double(const Numeric& value) {
			return value.integral ? static_cast<double>(value.integer) : value.physical;
		}

		EvaluatedValue evaluate_value(const Value& value, const Observation& observation, BindingMap& bindings) {
			switch (value.kind) {
			case Value::Kind::BOOLEAN: return value.boolean;
			case Value::Kind::NUMBER:
				return value.is_integer ? EvaluatedValue{integer_value(static_cast<uint64_t>(value.integer))}
										: EvaluatedValue{physical_value(value.number)};
			case Value::Kind::COLOR:        return value.color;
			case Value::Kind::BYTES:        return BytesRef{value.bytes};
			case Value::Kind::ENUM_LITERAL: return EnumRef{value.type, value.enum_value};
			case Value::Kind::COUNT:        return integer_value(evaluate_count(value, observation, bindings));
			case Value::Kind::NAME:
				if (value.type == ValueType::RPC_REQUEST || value.type == ValueType::RPC_RESPONSE ||
					value.type == ValueType::RPC_EVENT) {
					uint64_t id = value.entity_id;
					if (id == 0) {
						const auto found = bindings.find(value.text);
						id               = found == bindings.end() ? 0 : found->second;
					}
					if (id == 0) throw std::logic_error("unresolved typed LTL binding '" + value.text + "'");
					return RecordRef{value.type, id};
				}
				if (value.type == ValueType::MODULE || value.type == ValueType::TUB) {
					uint16_t id = static_cast<uint16_t>(value.entity_id);
					if (id == 0) {
						const auto found = bindings.find(value.text);
						id               = static_cast<uint16_t>(found == bindings.end() ? 0 : found->second);
					}
					if (id == 0) throw std::logic_error("unresolved typed LTL binding '" + value.text + "'");
					if (value.type == ValueType::MODULE) return ModuleRef{id};
					return TubRef{id};
				}
				break;
			case Value::Kind::CALL:
				if (value.symbol == Value::Symbol::EXIT_CAUSE || value.symbol == Value::Symbol::EXIT_ENDPOINT ||
					value.symbol == Value::Symbol::EXIT_MODULE || value.symbol == Value::Symbol::EXIT_TIME ||
					value.symbol == Value::Symbol::EXIT_DATA || value.symbol == Value::Symbol::EXIT_UID ||
					value.symbol == Value::Symbol::INITIAL_DATA || value.symbol == Value::Symbol::INITIAL_UID) {
					const TubRef tub = evaluate_tub(*value.arguments[0], observation, bindings);
					const bool   initial_fact =
						value.symbol == Value::Symbol::INITIAL_DATA || value.symbol == Value::Symbol::INITIAL_UID;
					observation.note(channel::tub(tub.id, initial_fact ? TubField::INITIAL : TubField::EXIT));
					if (initial_fact) {
						const TubSnapshot* initial = observation.initial_tub(tub.id);
						if (!initial)
							throw std::runtime_error("initial data for tub(" + std::to_string(tub.id) +
													 ") is unavailable before appearance");
						return value.symbol == Value::Symbol::INITIAL_UID ? BytesRef{initial->uid}
																		  : BytesRef{initial->data};
					}
					const TubExit* exit = observation.tub_exit(tub.id);
					if (!exit)
						throw std::runtime_error("exit facts for tub(" + std::to_string(tub.id) +
												 ") are unavailable before removal");
					if (value.symbol == Value::Symbol::EXIT_DATA) return BytesRef{exit->data};
					if (value.symbol == Value::Symbol::EXIT_UID) return BytesRef{exit->uid};
					if (value.symbol == Value::Symbol::EXIT_CAUSE)
						return integer_value(static_cast<uint8_t>(exit->cause));
					if (value.symbol == Value::Symbol::EXIT_MODULE) return integer_value(exit->source_module);
					if (value.symbol == Value::Symbol::EXIT_TIME) return integer_value(exit->tick);
					if (exit->endpoint < 0)
						throw std::runtime_error("exit endpoint is unavailable for a non-physical removal");
					return integer_value(static_cast<uint8_t>(exit->endpoint));
				}
				if (value.type == ValueType::RPC_RESPONSE) {
					const RecordRef request =
						std::get<RecordRef>(evaluate_value(*value.arguments[0], observation, bindings));
					return RecordRef{ValueType::RPC_RESPONSE, request.id};
				}
				if (value.type == ValueType::MODULE) return ModuleRef{static_cast<uint16_t>(value.entity_id)};
				if (value.type == ValueType::TUB) return TubRef{static_cast<uint16_t>(value.entity_id)};
				if (value.type == ValueType::BOOLEAN) return evaluate_call(value, observation, bindings);
				break;
			case Value::Kind::INDEX: {
				const BytesRef bytes = std::get<BytesRef>(evaluate_value(*value.base, observation, bindings));
				const int64_t  index =
					integer(evaluate_value(*value.arguments[0], observation, bindings), value.arguments[0]->span);
				if (index < 0 || static_cast<std::size_t>(index) >= bytes.size())
					throw std::runtime_error("byte index " + std::to_string(index) + " is outside sequence length " +
											 std::to_string(bytes.size()));
				return integer_value(bytes[static_cast<std::size_t>(index)]);
			}
			case Value::Kind::SLICE: {
				const BytesRef bytes = std::get<BytesRef>(evaluate_value(*value.base, observation, bindings));
				const int64_t  start =
					integer(evaluate_value(*value.arguments[0], observation, bindings), value.arguments[0]->span);
				const int64_t end =
					integer(evaluate_value(*value.arguments[1], observation, bindings), value.arguments[1]->span);
				const int64_t length    = static_cast<int64_t>(bytes.size());
				auto          normalize = [length](int64_t bound) {
					if (bound < 0) bound = std::max(-length, bound) + length;
					return std::clamp(bound, int64_t{0}, length);
				};
				const int64_t first = normalize(start), last = normalize(end);
				return last <= first
						   ? BytesRef{}
						   : bytes.subspan(static_cast<std::size_t>(first), static_cast<std::size_t>(last - first));
			}
			case Value::Kind::UNARY: {
				const EvaluatedValue operand = evaluate_value(*value.base, observation, bindings);
				if (value.operation == Value::Operator::BIT_NOT)
					return integer_value(~static_cast<uint64_t>(integer(operand, value.span)));
				const Numeric number = numeric(operand);
				if (!number.integral) return physical_value(-number.physical);
				return integer_value(0U - static_cast<uint64_t>(number.integer));
			}
			case Value::Kind::BINARY: {
				const Numeric left  = numeric(evaluate_value(*value.base, observation, bindings));
				const Numeric right = numeric(evaluate_value(*value.arguments[0], observation, bindings));
				if (value.operation == Value::Operator::ADD || value.operation == Value::Operator::SUBTRACT ||
					value.operation == Value::Operator::MULTIPLY || value.operation == Value::Operator::DIVIDE) {
					if (!left.integral || !right.integral) {
						const double a = as_double(left), b = as_double(right);
						if (value.operation == Value::Operator::DIVIDE && b == 0.0)
							throw std::runtime_error("division by zero");
						if (value.operation == Value::Operator::ADD) return physical_value(a + b);
						if (value.operation == Value::Operator::SUBTRACT) return physical_value(a - b);
						if (value.operation == Value::Operator::MULTIPLY) return physical_value(a * b);
						return physical_value(a / b);
					}
				}
				const uint64_t a = static_cast<uint64_t>(left.integer), b = static_cast<uint64_t>(right.integer);
				switch (value.operation) {
				case Value::Operator::ADD:      return integer_value(a + b);
				case Value::Operator::SUBTRACT: return integer_value(a - b);
				case Value::Operator::MULTIPLY: return integer_value(a * b);
				case Value::Operator::DIVIDE:
					if (right.integer == 0) throw std::runtime_error("division by zero");
					if (left.integer == std::numeric_limits<int64_t>::min() && right.integer == -1)
						return integer_value(a);
					return integer_value(static_cast<uint64_t>(left.integer / right.integer));
				case Value::Operator::MODULO:
					if (!left.integral || !right.integral) throw std::runtime_error("modulo requires integer operands");
					if (right.integer == 0) throw std::runtime_error("modulo by zero");
					if (left.integer == std::numeric_limits<int64_t>::min() && right.integer == -1)
						return integer_value(0);
					return integer_value(static_cast<uint64_t>(left.integer % right.integer));
				case Value::Operator::SHIFT_LEFT:
				case Value::Operator::SHIFT_RIGHT: {
					if (!left.integral || !right.integral || right.integer < 0 || right.integer >= 64)
						throw std::runtime_error("shift count must be an integer from 0 through 63");
					if (value.operation == Value::Operator::SHIFT_LEFT) return integer_value(a << right.integer);
					if (left.integer >= 0 || right.integer == 0) return integer_value(a >> right.integer);
					return integer_value((a >> right.integer) | (~uint64_t{0} << (64 - right.integer)));
				}
				case Value::Operator::BIT_AND: return integer_value(a & b);
				case Value::Operator::BIT_XOR: return integer_value(a ^ b);
				case Value::Operator::BIT_OR:  return integer_value(a | b);
				default:                       break;
				}
				throw std::logic_error("unknown numeric operator");
			}
			case Value::Kind::MEMBER: {
				if (value.base->type == ValueType::RPC_REQUEST || value.base->type == ValueType::RPC_RESPONSE ||
					value.base->type == ValueType::RPC_EVENT) {
					const RecordRef record = std::get<RecordRef>(evaluate_value(*value.base, observation, bindings));
					if (record.type == ValueType::RPC_REQUEST) {
						const RpcRequest* request = observation.rpc_request(record.id);
						if (!request) throw std::logic_error("unknown frozen RPC request");
						switch (value.symbol) {
						case Value::Symbol::ID:          return integer_value(request->id);
						case Value::Symbol::CALL_ID:     return integer_value(request->call_id);
						case Value::Symbol::MODULE:      return ModuleRef{request->module};
						case Value::Symbol::CMD:         return EnumRef{ValueType::RPC_COMMAND, request->cmd};
						case Value::Symbol::PAYLOAD:     return BytesRef{request->payload};
						case Value::Symbol::CONFIRMABLE: return request->confirmable;
						default:                         break;
						}
					}
					if (record.type == ValueType::RPC_RESPONSE) {
						const RpcRequest* request = observation.rpc_request(record.id);
						if (!request) throw std::logic_error("unknown correlated RPC request");
						switch (value.symbol) {
						case Value::Symbol::ID:      return integer_value(record.id);
						case Value::Symbol::REQUEST: return RecordRef{ValueType::RPC_REQUEST, record.id};
						case Value::Symbol::MODULE:  return ModuleRef{request->module};
						case Value::Symbol::CMD:     return EnumRef{ValueType::RPC_COMMAND, request->cmd};
						case Value::Symbol::STATUS:  {
							observation.note(channel::response(record.id));
							const RpcResponse* response = observation.rpc_response_to(record.id);
							return EnumRef{ValueType::RPC_STATUS, response ? response->status : RPC_STATUS_UNDELIVERED};
						}
						case Value::Symbol::PAYLOAD:
							observation.note(channel::response(record.id));
							if (const RpcResponse* response = observation.rpc_response_to(record.id)) {
								return BytesRef{response->payload};
							}
							return BytesRef{};
						default: break;
						}
					}
					if (record.type == ValueType::RPC_EVENT) {
						const RpcEvent* event = observation.rpc_event(record.id);
						if (!event) throw std::logic_error("unknown RPC event");
						switch (value.symbol) {
						case Value::Symbol::ID:      return integer_value(event->id);
						case Value::Symbol::MODULE:  return ModuleRef{event->module};
						case Value::Symbol::KIND:    return EnumRef{ValueType::RPC_EVENT_KIND, event->kind};
						case Value::Symbol::PAYLOAD: return BytesRef{event->payload};
						default:                     break;
						}
					}
					break;
				}
				if (value.symbol == Value::Symbol::LENGTH) {
					return integer_value(std::get<BytesRef>(evaluate_value(*value.base, observation, bindings)).size());
				}
				if (value.base->type == ValueType::TUB) {
					const TubRef tub = evaluate_tub(*value.base, observation, bindings);
					if (value.symbol == Value::Symbol::ID) return integer_value(tub.id);
					observation.note(channel::tub(tub.id, TubField::PRESENT));
					switch (value.symbol) {
					case Value::Symbol::MODULE: observation.note(channel::tub(tub.id, TubField::MODULE)); break;
					case Value::Symbol::BELT:   observation.note(channel::tub(tub.id, TubField::BELT)); break;
					case Value::Symbol::POSITION_UM:
						observation.note(channel::tub(tub.id, TubField::POSITION_UM));
						break;
					case Value::Symbol::PREVIOUS_MODULE:
						observation.note(channel::tub(tub.id, TubField::PREVIOUS_MODULE));
						break;
					case Value::Symbol::UID: observation.note(channel::tub(tub.id, TubField::UID)); break;
					default:                 observation.note(channel::tub(tub.id, TubField::DATA)); break;
					}
					const auto found = observation.tubs().find(tub.id);
					if (found == observation.tubs().end())
						throw std::runtime_error("tub(" + std::to_string(tub.id) +
												 ") data is unavailable while the tub is not present");
					if (value.symbol == Value::Symbol::MODULE) return ModuleRef{found->second.module};
					if (value.symbol == Value::Symbol::BELT)
						return integer_value(static_cast<uint8_t>(found->second.belt));
					if (value.symbol == Value::Symbol::POSITION_UM) return integer_value(found->second.position_um);
					if (value.symbol == Value::Symbol::PREVIOUS_MODULE)
						return integer_value(found->second.previous_module);
					return value.symbol == Value::Symbol::UID ? BytesRef{found->second.uid}
															  : BytesRef{found->second.data};
				}
				const ModuleRef module = evaluate_module(*value.base, observation, bindings);
				if (value.symbol == Value::Symbol::ID) return integer_value(module.id);
				if (value.symbol == Value::Symbol::ENTRY) return PositionRef{module.id, world::Position::ENTRY};
				if (value.symbol == Value::Symbol::CENTER) return PositionRef{module.id, world::Position::CENTER};
				if (value.symbol == Value::Symbol::LEFT_LASER)
					return PositionRef{module.id, world::Position::LEFT_LASER};
				if (value.symbol == Value::Symbol::RIGHT_LASER)
					return PositionRef{module.id, world::Position::RIGHT_LASER};
				const ModuleSnapshot& state = module_snapshot(observation, module.id);
				if (value.symbol != Value::Symbol::LEG_LENGTH_UM && value.symbol != Value::Symbol::TUB_RADIUS_UM)
					observation.note(channel::module_field(module.id, module_field(value.symbol)));
				switch (value.symbol) {
				case Value::Symbol::LEG_LENGTH_UM:        return integer_value(state.leg_length_um);
				case Value::Symbol::TUB_RADIUS_UM:        return integer_value(state.tub_radius_um);
				case Value::Symbol::BIG_BELT_SPEED:       return physical_value(state.big_belt_speed);
				case Value::Symbol::SMALL_BELT_SPEED:     return physical_value(state.small_belt_speed);
				case Value::Symbol::SERVO_ANGLE:          return physical_value(state.servo_angle);
				case Value::Symbol::SERVO_PHYSICAL_ANGLE: return physical_value(state.servo_physical_angle);
				case Value::Symbol::LED_COLOR:            return state.led_color;
				case Value::Symbol::LED_VALUE:            return integer_value(state.led_color);
				case Value::Symbol::LEFT_LASER_ENABLED:   return state.left_laser_enabled;
				case Value::Symbol::RIGHT_LASER_ENABLED:  return state.right_laser_enabled;
				case Value::Symbol::LEFT_LASER_BLOCKED:   return state.left_laser_blocked;
				case Value::Symbol::RIGHT_LASER_BLOCKED:  return state.right_laser_blocked;
				case Value::Symbol::RFID_ACTIVE:          return state.rfid_active;
				default:                                  break;
				}
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
				case Formula::Comparison::EQUAL:     return equal(left, right, formula.value->type);
				case Formula::Comparison::NOT_EQUAL: return !equal(left, right, formula.value->type);
				case Formula::Comparison::LESS:
					return as_double(std::get<Numeric>(left)) < as_double(std::get<Numeric>(right));
				case Formula::Comparison::LESS_EQUAL:
					return as_double(std::get<Numeric>(left)) <= as_double(std::get<Numeric>(right));
				case Formula::Comparison::GREATER:
					return as_double(std::get<Numeric>(left)) > as_double(std::get<Numeric>(right));
				case Formula::Comparison::GREATER_EQUAL:
					return as_double(std::get<Numeric>(left)) >= as_double(std::get<Numeric>(right));
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
			switch (type) {
			case ValueType::BOOLEAN: return std::get<bool>(left) == std::get<bool>(right);
			case ValueType::NUMBER:  {
				const Numeric a = std::get<Numeric>(left), b = std::get<Numeric>(right);
				if (a.integral && b.integral) return a.integer == b.integer;
				return std::fabs(as_double(a) - as_double(b)) <= NUMERIC_TOLERANCE;
			}
			case ValueType::COLOR:          return std::get<uint32_t>(left) == std::get<uint32_t>(right);
			case ValueType::MODULE:         return std::get<ModuleRef>(left) == std::get<ModuleRef>(right);
			case ValueType::TUB:            return std::get<TubRef>(left) == std::get<TubRef>(right);
			case ValueType::POSITION:       return std::get<PositionRef>(left) == std::get<PositionRef>(right);
			case ValueType::BYTES:          return std::ranges::equal(std::get<BytesRef>(left), std::get<BytesRef>(right));
			case ValueType::RPC_COMMAND:
			case ValueType::RPC_STATUS:
			case ValueType::RPC_EVENT_KIND: return std::get<EnumRef>(left) == std::get<EnumRef>(right);
			case ValueType::RPC_REQUEST:
			case ValueType::RPC_RESPONSE:
			case ValueType::RPC_EVENT:      return std::get<RecordRef>(left) == std::get<RecordRef>(right);
			case ValueType::UNKNOWN:        break;
			}
			throw std::logic_error("cannot compare untyped LTL values");
		}

		std::string render_value(const EvaluatedValue& value, ValueType type) {
			std::ostringstream output;
			if (const auto boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
			if (const auto number = std::get_if<Numeric>(&value)) {
				if (number->integral) output << number->integer;
				else output << std::setprecision(17) << number->physical;
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
			if (const auto bytes = std::get_if<BytesRef>(&value)) {
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

	void ModuleSteps::emplace(uint16_t module, bool stepped) {
		if (module >= channel::ENTITIES)
			throw std::logic_error("module " + std::to_string(module) + " has no step state");
		if (states_[module] == NONE) states_[module] = stepped ? STEPPED : SKIPPED;
	}

	Observer::Observer(const std::vector<module::ModuleId>& modules, const world::State& baseline)
		: module_ids_(modules), previous_modules_(snapshot_modules(modules, baseline)) {}

	Observation Observer::observe(uint64_t logical_tick, const world::State& world, std::vector<RpcRequest> requests,
								  std::vector<RpcResponse> responses, std::vector<RpcEvent> events,
								  const ModuleSteps& module_stepped, const module::Changes* changes) {
		Observation observation;
		observation.tick_           = logical_tick;
		observation.timestamp_      = std::chrono::milliseconds(logical_tick);
		observation.module_stepped_ = module_stepped;
		observation.modules_        = snapshot_modules(module_ids_, world, previous_modules_, changes);
		observation.tubs_           = snapshot_tubs(world, previous_tubs_);
		static const TubSnapshots no_tubs;
		const TubSnapshots&       tubs     = *observation.tubs_;
		const TubSnapshots&       previous = previous_tubs_ ? *previous_tubs_ : no_tubs;
		// Shared snapshots have the same tubs, so they need no tub bookkeeping.
		const bool tubs_changed = observation.tubs_ != previous_tubs_;
		if (tubs_changed) {
			std::shared_ptr<std::set<uint16_t>> seen;
			for (const auto& [id, tub] : tubs) {
				(void)tub;
				if (previous.contains(id)) continue;
				if (seen_tubs_->contains(id)) {
					throw std::logic_error("tub " + std::to_string(id) + " was reintroduced after removal");
				}
				if (!seen) seen = std::make_shared<std::set<uint16_t>>(*seen_tubs_);
				seen->insert(id);
			}
			if (seen) seen_tubs_ = std::move(seen);
			for (const auto& [id, tub] : tubs) initial_tub_registry_.try_emplace(id, tub);
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
			if (response.tick > logical_tick) time_sensitive_records_ = true;
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
			if (event.tick > logical_tick) time_sensitive_records_ = true;
			if (!rpc_event_registry_.emplace(event.id, event).second) {
				throw std::logic_error("duplicate RPC event identity " + std::to_string(event.id));
			}
		}
		if (tubs_changed)
			derive_tub_edges(tubs, previous, observation.appeared_, observation.disappeared_, observation.entered_,
							 observation.left_);
		for (const uint16_t id : observation.disappeared_) {
			const TubSnapshot& before = previous.at(id);
			TubExit            exit;
			exit.tub           = id;
			exit.source_module = before.module;
			exit.cause = world.removal_reason(static_cast<uint8_t>(id)).value_or(world::TubRemovalReason::EXPLICIT);
			exit.tick  = logical_tick;
			exit.uid   = before.uid;
			exit.data  = before.data;
			if (exit.cause == world::TubRemovalReason::YEETED) {
				exit.endpoint = before.belt == world::Belt::SMALL ? 1 : (before.position_um == 0 ? 2 : 0);
			}
			tub_exit_registry_.emplace(id, std::move(exit));
		}
		if (observation.modules_ != previous_modules_)
			derive_module_edges(*observation.modules_, *previous_modules_, observation.changed_);
		derive_channel_changes(observation);
		// Every exposed tub field participates in the revision, including continuous position
		// and the clearing of the one-observation physical-transfer source.
		bool tub_change = false;
		if (tubs_changed) {
			tub_change = tubs.size() != previous.size();
			for (const auto& [id, tub] : tubs) {
				const auto before = previous.find(id);
				if (before == previous.end() || tub != before->second) tub_change = true;
			}
		}
		const bool pulses = !observation.changed_.empty() || !observation.appeared_.empty() ||
							!observation.disappeared_.empty() || !observation.entered_.empty() ||
							!observation.left_.empty() || !observation.rpc_requests_.empty() ||
							!observation.rpc_responses_.empty() || !observation.rpc_events_.empty();
		// Out-of-order observations and future-dated records need time-dependent visibility.
		// Keep the conservative per-observation path for these uncommon callers.
		if (revision_ && logical_tick <= previous_tick_) time_sensitive_records_ = true;
		if (!revision_ || pulses || previous_pulses_ || tub_change || time_sensitive_records_ ||
			observation.module_stepped_ != previous_stepped_) {
			static std::atomic<uint64_t> next_revision{1};
			revision_ = next_revision.fetch_add(1, std::memory_order_relaxed);
		}
		observation.revision_ = revision_;
		previous_tick_        = logical_tick;
		previous_pulses_      = pulses;
		previous_stepped_     = observation.module_stepped_;
		previous_tubs_        = observation.tubs_;
		previous_modules_     = observation.modules_;
		return observation;
	}

	void Observer::mark_tub(uint16_t id, uint64_t tick) {
		for (uint8_t field = 0; field <= static_cast<uint8_t>(channel::TubField::INITIAL); field++)
			mark(channel::tub(id, static_cast<channel::TubField>(field)), tick);
	}

	// Runs before the previous_* state advances. A pulse channel changes on the observation it
	// fires and on the next one, where it clears.
	void Observer::derive_channel_changes(const Observation& observation) {
		using channel::TubField;
		const uint64_t tick = observation.tick_;
		changed_now_.clear();
		mark(channel::VOLATILE, tick);

		if (observation.modules_ != previous_modules_) {
			for (const auto& [id, current] : *observation.modules_) {
				const ModuleSnapshot& before = previous_modules_->at(id);
				if (current == before) continue;
				for (uint8_t field = 0; field <= static_cast<uint8_t>(ModuleField::RFID_ACTIVE); field++) {
					const auto typed = static_cast<ModuleField>(field);
					if (different(typed, current, before)) mark(channel::module_field(id, typed), tick);
				}
			}
		}
		for (const ChangedField& edge : observation.changed_) mark(channel::module_edge(edge.module, edge.field), tick);
		for (const ChangedField& edge : previous_changed_) mark(channel::module_edge(edge.module, edge.field), tick);
		previous_changed_ = observation.changed_;

		static const std::map<uint16_t, TubSnapshot> no_tubs;
		const auto&                                  tubs     = observation.tubs();
		const auto&                                  previous = previous_tubs_ ? *previous_tubs_ : no_tubs;
		// Shared snapshots are equal, so none of their tub fields changed.
		const bool tubs_changed = observation.tubs_ != previous_tubs_;
		for (const auto& [id, tub] : tubs_changed ? tubs : no_tubs) {
			const auto before = previous.find(id);
			if (before == previous.end()) {
				mark_tub(id, tick);
				continue;
			}
			const TubSnapshot& old = before->second;
			if (tub.module != old.module) mark(channel::tub(id, TubField::MODULE), tick);
			if (tub.belt != old.belt) mark(channel::tub(id, TubField::BELT), tick);
			if (tub.position_um != old.position_um) mark(channel::tub(id, TubField::POSITION_UM), tick);
			if (tub.position != old.position) mark(channel::tub(id, TubField::POSITION), tick);
			if (tub.previous_module != old.previous_module) mark(channel::tub(id, TubField::PREVIOUS_MODULE), tick);
			if (tub.uid != old.uid) mark(channel::tub(id, TubField::UID), tick);
			if (tub.data != old.data) mark(channel::tub(id, TubField::DATA), tick);
		}
		for (const auto& [id, tub] : tubs_changed ? previous : no_tubs) {
			(void)tub;
			if (!tubs.contains(id)) mark_tub(id, tick);
		}
		// Appearance, disappearance, exits, and the first sighting change the whole tub record, and
		// their pulses clear on the next observation.
		for (const uint16_t id : observation.appeared_) mark_tub(id, tick);
		for (const uint16_t id : observation.disappeared_) mark_tub(id, tick);
		for (const uint16_t id : previous_appeared_) mark_tub(id, tick);
		for (const uint16_t id : previous_disappeared_) mark_tub(id, tick);
		previous_appeared_    = observation.appeared_;
		previous_disappeared_ = observation.disappeared_;
		std::set<uint16_t> edge_tubs;
		for (const TubPosition& edge : observation.entered_) edge_tubs.insert(edge.tub);
		for (const TubPosition& edge : observation.left_) edge_tubs.insert(edge.tub);
		for (const uint16_t id : edge_tubs) mark(channel::tub(id, TubField::EDGES), tick);
		for (const uint16_t id : previous_edge_tubs_) mark(channel::tub(id, TubField::EDGES), tick);
		previous_edge_tubs_ = std::move(edge_tubs);
		if (observation.known_tubs_->size() != seen_count_) {
			seen_count_ = observation.known_tubs_->size();
			mark(channel::TUB_DOMAIN, tick);
		}

		// Wake only subscribers of the record kind actually produced, including its clearing tick.
		// A response must not invalidate a module's request-only or event-only predicates.
		std::vector<uint32_t> record_channels;
		for (const RpcRequest& request : observation.rpc_requests_)
			record_channels.push_back(channel::records(request.module, channel::RecordList::REQUESTS));
		for (const RpcResponse& response : observation.rpc_responses_) {
			record_channels.push_back(channel::records(response.module, channel::RecordList::RESPONSES));
			mark(channel::response(response.request_id), tick);
		}
		for (const RpcEvent& event : observation.rpc_events_)
			record_channels.push_back(channel::records(event.module, channel::RecordList::EVENTS));
		deduplicate(record_channels);
		for (const uint32_t channel : record_channels) mark(channel, tick);
		for (const uint32_t channel : previous_record_channels_) mark(channel, tick);
		previous_record_channels_ = std::move(record_channels);

		if (observation.module_stepped_ != previous_stepped_) {
			// Changed entries first, then removed ones, each in module order.
			for (uint16_t id = 0; id < channel::ENTITIES; id++) {
				const auto stepped = observation.module_stepped_.find(id);
				if (stepped && previous_stepped_.find(id) != stepped) mark(channel::stepped(id), tick);
			}
			for (uint16_t id = 0; id < channel::ENTITIES; id++) {
				if (!observation.module_stepped_.find(id) && previous_stepped_.find(id))
					mark(channel::stepped(id), tick);
			}
		}
	}

	bool Observer::changed_since(const std::vector<uint32_t>& channels, uint64_t tick) const {
		if (time_sensitive_records_) return true;
		for (const uint32_t channel : channels)
			if (changed_tick_[channel] > tick) return true;
		return false;
	}

	void Observer::deduplicate(std::vector<uint32_t>& channels) const {
		if (++channel_generation_ == 0) {
			std::ranges::fill(channel_stamps_, 0);
			channel_generation_ = 1;
		}
		std::erase_if(channels, [this](uint32_t channel) {
			if (channel_stamps_[channel] == channel_generation_) return true;
			channel_stamps_[channel] = channel_generation_;
			return false;
		});
	}

	void Observation::deduplicate(std::vector<uint32_t>& channels) const {
		if (observer_) observer_->deduplicate(channels);
		else {
			std::ranges::sort(channels);
			const auto duplicates = std::ranges::unique(channels);
			channels.erase(duplicates.begin(), duplicates.end());
		}
	}

	const std::vector<uint32_t>& Observation::changed_channels() const {
		static const std::vector<uint32_t> none;
		return observer_ ? observer_->changed_channels() : none;
	}

	bool Observation::volatile_inputs() const {
		return !observer_ || observer_->volatile_inputs();
	}

	bool Observation::changed_since(const std::vector<uint32_t>& channels, uint64_t tick) const {
		return !observer_ || observer_->changed_since(channels, tick);
	}

	bool Observation::evaluate(const Formula& atom) const {
		BindingMap bindings;
		return evaluate_formula(atom, *this, bindings);
	}

	bool Observation::evaluate(const Formula& atom, const std::vector<std::pair<std::string, uint64_t>>& bound) const {
		BindingMap bindings;
		for (const auto& [name, identity] : bound) bindings[name] = identity;
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
			note(channel::records(module.id, type == ValueType::RPC_REQUEST    ? channel::RecordList::REQUESTS
											 : type == ValueType::RPC_RESPONSE ? channel::RecordList::RESPONSES
																			   : channel::RecordList::EVENTS));
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
			for (const auto& [id, module] : modules()) {
				(void)module;
				visit(id);
			}
			return;
		}
		if (type == ValueType::TUB) {
			note(channel::TUB_DOMAIN);
			if (known_tubs_)
				for (const uint16_t id : *known_tubs_) visit(id);
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

	const TubExit* Observer::tub_exit(uint16_t id, uint64_t observation_tick) const {
		const auto found = tub_exit_registry_.find(id);
		return found == tub_exit_registry_.end() || found->second.tick > observation_tick ? nullptr : &found->second;
	}

	const TubExit* Observation::tub_exit(uint16_t id) const {
		return observer_ ? observer_->tub_exit(id, tick_) : nullptr;
	}

	const TubSnapshot* Observer::initial_tub(uint16_t id) const {
		const auto found = initial_tub_registry_.find(id);
		return found == initial_tub_registry_.end() ? nullptr : &found->second;
	}

	const TubSnapshot* Observation::initial_tub(uint16_t id) const {
		return observer_ ? observer_->initial_tub(id) : nullptr;
	}

} // namespace ltl
