#include "checker.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "../defs.h"
#include "rpc_vocabulary.h"

namespace ltl {

	namespace {

		class Checker {
		private:
			std::string                                filename_;
			std::unordered_set<uint16_t>               modules_;
			std::unordered_set<std::string>            names_;
			std::unordered_map<std::string, ValueType> bindings_;

			[[noreturn]] void error(const Span& span, const std::string& message) const {
				const std::string& source = span.begin.source.empty() ? filename_ : span.begin.source;
				throw std::invalid_argument(source + ":" + std::to_string(span.begin.line) + ":" +
											std::to_string(span.begin.column) + ": " + message);
			}

			static std::string type_name(ValueType type) {
				switch (type) {
				case ValueType::UNKNOWN:        return "unknown";
				case ValueType::BOOLEAN:        return "Boolean";
				case ValueType::NUMBER:         return "Number";
				case ValueType::COLOR:          return "Color";
				case ValueType::MODULE:         return "Module";
				case ValueType::TUB:            return "Tub";
				case ValueType::POSITION:       return "Position";
				case ValueType::BYTES:          return "Bytes";
				case ValueType::RPC_COMMAND:    return "RpcCommand";
				case ValueType::RPC_STATUS:     return "RpcStatus";
				case ValueType::RPC_EVENT_KIND: return "RpcEventKind";
				case ValueType::RPC_REQUEST:    return "RpcRequest";
				case ValueType::RPC_RESPONSE:   return "RpcResponse";
				case ValueType::RPC_EVENT:      return "RpcEvent";
				}
				return "unknown";
			}

			static bool integral(double value) noexcept { return std::floor(value) == value; }

			static bool snake_case(const std::string& name) {
				if (name.empty() || (name.front() != '_' && (name.front() < 'a' || name.front() > 'z'))) return false;
				for (const char character : name) {
					if (character != '_' && (character < 'a' || character > 'z') &&
						(character < '0' || character > '9'))
						return false;
				}
				return true;
			}

			void require_arity(const Value& value, std::size_t expected) const {
				if (value.arguments.size() != expected) {
					error(value.span, value.text + " expects " + std::to_string(expected) + " argument" +
										  (expected == 1 ? "" : "s"));
				}
			}

			ValueType check_entity(Value& value, ValueType entity) {
				require_arity(value, 1);
				Value& argument = *value.arguments[0];
				if (check(argument) != ValueType::NUMBER || !integral(argument.number)) {
					error(argument.span, value.text + " id must be an integer literal");
				}
				if (entity == ValueType::MODULE) {
					const bool routing = argument.number >= 1 && argument.number < MAX_MODULES;
					if (!routing && argument.number != TOWER_MODULE_ID) {
						error(argument.span, "module id must be between 1 and " + std::to_string(MAX_MODULES - 1) +
												 ", or the tower id " + std::to_string(TOWER_MODULE_ID));
					}
					if (!modules_.contains(static_cast<uint16_t>(argument.number))) {
						error(argument.span, "module " + std::to_string(static_cast<uint16_t>(argument.number)) +
												 " does not exist in the selected layout");
					}
				} else if (argument.number < 1 || argument.number > std::numeric_limits<uint8_t>::max()) {
					error(argument.span, "tub id must be between 1 and 255");
				}
				value.type      = entity;
				value.entity_id = static_cast<uint16_t>(argument.number);
				return entity;
			}

			ValueType check_call(Value& value) {
				if (value.text == "module") return check_entity(value, ValueType::MODULE);
				if (value.text == "tub") return check_entity(value, ValueType::TUB);

				if (value.text == "rpc_response_to") {
					require_arity(value, 1);
					if (check(*value.arguments[0]) != ValueType::RPC_REQUEST) {
						error(value.arguments[0]->span, "rpc_response_to expects an RpcRequest");
					}
					value.type = ValueType::RPC_RESPONSE;
					return value.type;
				}

				if (value.text == "present") {
					value.symbol = Value::Symbol::PRESENT;
					require_arity(value, 1);
					if (check(*value.arguments[0]) != ValueType::TUB) {
						error(value.arguments[0]->span, "present expects a Tub");
					}
				} else if (value.text == "at_module") {
					value.symbol = Value::Symbol::AT_MODULE;
					require_arity(value, 2);
					if (check(*value.arguments[0]) != ValueType::TUB)
						error(value.arguments[0]->span, "at_module expects a Tub first");
					if (check(*value.arguments[1]) != ValueType::MODULE) {
						error(value.arguments[1]->span, "at_module expects a Module second");
					}
				} else if (value.text == "at_position") {
					value.symbol = Value::Symbol::AT_POSITION;
					require_arity(value, 2);
					if (check(*value.arguments[0]) != ValueType::TUB) {
						error(value.arguments[0]->span, "at_position expects a Tub first");
					}
					if (check(*value.arguments[1]) != ValueType::POSITION) {
						error(value.arguments[1]->span, "at_position expects a Position second");
					}
				} else if (value.text == "appeared" || value.text == "disappeared") {
					value.symbol = value.text == "appeared" ? Value::Symbol::APPEARED : Value::Symbol::DISAPPEARED;
					require_arity(value, 1);
					if (check(*value.arguments[0]) != ValueType::TUB) {
						error(value.arguments[0]->span, value.text + " expects a Tub");
					}
				} else if (value.text == "physical_exit" || value.text == "exit_cause" ||
						   value.text == "exit_endpoint" || value.text == "exit_module" || value.text == "exit_time" ||
						   value.text == "exit_data" || value.text == "exit_uid" || value.text == "initial_data" ||
						   value.text == "initial_uid") {
					require_arity(value, 1);
					if (check(*value.arguments[0]) != ValueType::TUB)
						error(value.arguments[0]->span, value.text + " expects a Tub");
					if (value.text == "physical_exit") {
						value.symbol = Value::Symbol::PHYSICAL_EXIT;
						value.type   = ValueType::BOOLEAN;
					} else if (value.text == "exit_cause") {
						value.symbol = Value::Symbol::EXIT_CAUSE;
						value.type   = ValueType::NUMBER;
					} else if (value.text == "exit_endpoint") {
						value.symbol = Value::Symbol::EXIT_ENDPOINT;
						value.type   = ValueType::NUMBER;
					} else if (value.text == "exit_module") {
						value.symbol = Value::Symbol::EXIT_MODULE;
						value.type   = ValueType::NUMBER;
					} else if (value.text == "exit_time") {
						value.symbol = Value::Symbol::EXIT_TIME;
						value.type   = ValueType::NUMBER;
					} else if (value.text == "exit_data") {
						value.symbol = Value::Symbol::EXIT_DATA;
						value.type   = ValueType::BYTES;
					} else if (value.text == "exit_uid") {
						value.symbol = Value::Symbol::EXIT_UID;
						value.type   = ValueType::BYTES;
					} else if (value.text == "initial_data") {
						value.symbol = Value::Symbol::INITIAL_DATA;
						value.type   = ValueType::BYTES;
					} else {
						value.symbol = Value::Symbol::INITIAL_UID;
						value.type   = ValueType::BYTES;
					}
					return value.type;
				} else if (value.text == "entered" || value.text == "left") {
					value.symbol = value.text == "entered" ? Value::Symbol::ENTERED : Value::Symbol::LEFT;
					require_arity(value, 2);
					if (check(*value.arguments[0]) != ValueType::TUB)
						error(value.arguments[0]->span, value.text + " expects a Tub first");
					if (check(*value.arguments[1]) != ValueType::POSITION) {
						error(value.arguments[1]->span, value.text + " expects a Position second");
					}
				} else if (value.text == "changed") {
					value.symbol = Value::Symbol::CHANGED;
					require_arity(value, 1);
					Value& field = *value.arguments[0];
					if (field.kind != Value::Kind::MEMBER) {
						error(field.span, "changed expects a module output field");
					}
					const ValueType type = check(field);
					if (field.base->type != ValueType::MODULE || type == ValueType::POSITION) {
						error(field.span, "changed expects a module output field");
					}
				} else {
					error(value.span, "unknown function '" + value.text + "'");
				}
				value.type = ValueType::BOOLEAN;
				return value.type;
			}

			static ValueType resolve(Value& value, ValueType type, Value::Symbol symbol) {
				value.type   = type;
				value.symbol = symbol;
				return type;
			}

			ValueType check_member(Value& value) {
				const ValueType base = check(*value.base);
				if (base == ValueType::BYTES) {
					if (value.text == "length") return resolve(value, ValueType::NUMBER, Value::Symbol::LENGTH);
					error(value.span, "unknown Bytes member '" + value.text + "'");
				}
				if (base == ValueType::TUB) {
					if (value.text == "data") return resolve(value, ValueType::BYTES, Value::Symbol::DATA);
					if (value.text == "uid") return resolve(value, ValueType::BYTES, Value::Symbol::UID);
					error(value.span, "unknown Tub member '" + value.text + "'");
				}
				if (base == ValueType::RPC_REQUEST) {
					if (value.text == "id") return resolve(value, ValueType::NUMBER, Value::Symbol::ID);
					if (value.text == "call_id") return resolve(value, ValueType::NUMBER, Value::Symbol::CALL_ID);
					if (value.text == "module") return resolve(value, ValueType::MODULE, Value::Symbol::MODULE);
					if (value.text == "cmd") return resolve(value, ValueType::RPC_COMMAND, Value::Symbol::CMD);
					if (value.text == "payload") return resolve(value, ValueType::BYTES, Value::Symbol::PAYLOAD);
					if (value.text == "confirmable")
						return resolve(value, ValueType::BOOLEAN, Value::Symbol::CONFIRMABLE);
					error(value.span, "unknown RpcRequest member '" + value.text + "'");
				}
				if (base == ValueType::RPC_RESPONSE) {
					if (value.text == "id") return resolve(value, ValueType::NUMBER, Value::Symbol::ID);
					if (value.text == "request") return resolve(value, ValueType::RPC_REQUEST, Value::Symbol::REQUEST);
					if (value.text == "module") return resolve(value, ValueType::MODULE, Value::Symbol::MODULE);
					if (value.text == "cmd") return resolve(value, ValueType::RPC_COMMAND, Value::Symbol::CMD);
					if (value.text == "status") return resolve(value, ValueType::RPC_STATUS, Value::Symbol::STATUS);
					if (value.text == "payload") return resolve(value, ValueType::BYTES, Value::Symbol::PAYLOAD);
					error(value.span, "unknown RpcResponse member '" + value.text + "'");
				}
				if (base == ValueType::RPC_EVENT) {
					if (value.text == "id") return resolve(value, ValueType::NUMBER, Value::Symbol::ID);
					if (value.text == "module") return resolve(value, ValueType::MODULE, Value::Symbol::MODULE);
					if (value.text == "kind") return resolve(value, ValueType::RPC_EVENT_KIND, Value::Symbol::KIND);
					if (value.text == "payload") return resolve(value, ValueType::BYTES, Value::Symbol::PAYLOAD);
					error(value.span, "unknown RpcEvent member '" + value.text + "'");
				}
				if (base != ValueType::MODULE) error(value.base->span, "this value has no members");

				if (value.text == "id") return resolve(value, ValueType::NUMBER, Value::Symbol::ID);
				if (value.text == "big_belt_speed")
					return resolve(value, ValueType::NUMBER, Value::Symbol::BIG_BELT_SPEED);
				if (value.text == "small_belt_speed")
					return resolve(value, ValueType::NUMBER, Value::Symbol::SMALL_BELT_SPEED);
				if (value.text == "servo_angle") return resolve(value, ValueType::NUMBER, Value::Symbol::SERVO_ANGLE);
				if (value.text == "servo_physical_angle")
					return resolve(value, ValueType::NUMBER, Value::Symbol::SERVO_PHYSICAL_ANGLE);
				if (value.text == "led_color") return resolve(value, ValueType::COLOR, Value::Symbol::LED_COLOR);
				if (value.text == "left_laser_enabled")
					return resolve(value, ValueType::BOOLEAN, Value::Symbol::LEFT_LASER_ENABLED);
				if (value.text == "right_laser_enabled")
					return resolve(value, ValueType::BOOLEAN, Value::Symbol::RIGHT_LASER_ENABLED);
				if (value.text == "left_laser_blocked")
					return resolve(value, ValueType::BOOLEAN, Value::Symbol::LEFT_LASER_BLOCKED);
				if (value.text == "right_laser_blocked")
					return resolve(value, ValueType::BOOLEAN, Value::Symbol::RIGHT_LASER_BLOCKED);
				if (value.text == "rfid_active") return resolve(value, ValueType::BOOLEAN, Value::Symbol::RFID_ACTIVE);
				if (value.text == "entry") return resolve(value, ValueType::POSITION, Value::Symbol::ENTRY);
				if (value.text == "center") return resolve(value, ValueType::POSITION, Value::Symbol::CENTER);
				if (value.text == "left_laser") return resolve(value, ValueType::POSITION, Value::Symbol::LEFT_LASER);
				if (value.text == "right_laser") return resolve(value, ValueType::POSITION, Value::Symbol::RIGHT_LASER);
				error(value.span, "unknown Module member '" + value.text + "'");
			}

			void enter_binding(const std::string& name, ValueType type, const Span& span) {
				if (!snake_case(name)) error(span, "bound names must use snake_case");
				if (bindings_.contains(name)) error(span, "bound name '" + name + "' shadows an existing binding");
				bindings_.emplace(name, type);
			}

			ValueType check_domain(Value& domain) {
				if (domain.kind != Value::Kind::CALL ||
					(domain.text != "rpc_requests" && domain.text != "rpc_responses" && domain.text != "rpc_events")) {
					error(domain.span, "expected an RPC domain expression");
				}
				require_arity(domain, 1);
				if (check(*domain.arguments[0]) != ValueType::MODULE) {
					error(domain.arguments[0]->span, domain.text + " expects a Module");
				}
				if (domain.text == "rpc_requests") domain.type = ValueType::RPC_REQUEST;
				else if (domain.text == "rpc_responses") domain.type = ValueType::RPC_RESPONSE;
				else domain.type = ValueType::RPC_EVENT;
				return domain.type;
			}

			ValueType check_count(Value& value) {
				if (value.domain) value.binding_type = check_domain(*value.domain);
				if (value.binding_type != ValueType::MODULE && value.binding_type != ValueType::TUB &&
					value.binding_type != ValueType::RPC_REQUEST && value.binding_type != ValueType::RPC_RESPONSE &&
					value.binding_type != ValueType::RPC_EVENT) {
					error(value.span, "count requires a finite typed domain");
				}
				enter_binding(value.binding, value.binding_type, value.span);
				check_formula(*value.predicate, true);
				bindings_.erase(value.binding);
				value.type = ValueType::NUMBER;
				return value.type;
			}

			static bool enum_type(ValueType type) noexcept {
				return type == ValueType::RPC_COMMAND || type == ValueType::RPC_STATUS ||
					   type == ValueType::RPC_EVENT_KIND;
			}

			void validate_enum(Value& value) const {
				if (value.kind != Value::Kind::ENUM_LITERAL) return;
				std::optional<int64_t> resolved;
				if (value.type == ValueType::RPC_COMMAND) resolved = enum_value(rpc_commands(), value.text);
				else if (value.type == ValueType::RPC_STATUS) resolved = enum_value(rpc_statuses(), value.text);
				else if (value.type == ValueType::RPC_EVENT_KIND) resolved = enum_value(rpc_event_kinds(), value.text);
				if (!resolved) error(value.span, "unknown " + type_name(value.type) + " literal '." + value.text + "'");
				value.enum_value = *resolved;
			}

			ValueType check(Value& value, ValueType expected = ValueType::UNKNOWN) {
				switch (value.kind) {
				case Value::Kind::BOOLEAN: value.type = ValueType::BOOLEAN; break;
				case Value::Kind::NUMBER:  value.type = ValueType::NUMBER; break;
				case Value::Kind::COLOR:   value.type = ValueType::COLOR; break;
				case Value::Kind::BYTES:   value.type = ValueType::BYTES; break;
				case Value::Kind::ENUM_LITERAL:
					if (enum_type(expected)) value.type = expected;
					break;
				case Value::Kind::COUNT:  return check_count(value);
				case Value::Kind::CALL:   return check_call(value);
				case Value::Kind::MEMBER: return check_member(value);
				case Value::Kind::INDEX:
					if (check(*value.base) != ValueType::BYTES) error(value.base->span, "indexing requires Bytes");
					if (value.arguments.size() != 1 || check(*value.arguments[0]) != ValueType::NUMBER)
						error(value.span, "byte index must be a Number");
					value.type = ValueType::NUMBER;
					return value.type;
				case Value::Kind::SLICE:
					if (check(*value.base) != ValueType::BYTES) error(value.base->span, "slicing requires Bytes");
					if (value.arguments.size() != 2 || check(*value.arguments[0]) != ValueType::NUMBER ||
						check(*value.arguments[1]) != ValueType::NUMBER)
						error(value.span, "byte slice bounds must be Numbers");
					value.type = ValueType::BYTES;
					return value.type;
				case Value::Kind::UNARY:
					if (check(*value.base) != ValueType::NUMBER)
						error(value.base->span, "numeric unary operator requires a Number");
					value.type = ValueType::NUMBER;
					return value.type;
				case Value::Kind::BINARY:
					if (check(*value.base) != ValueType::NUMBER || check(*value.arguments[0]) != ValueType::NUMBER)
						error(value.span, "arithmetic and bitwise operators require Numbers");
					value.type = ValueType::NUMBER;
					return value.type;
				case Value::Kind::NAME: {
					const auto found = bindings_.find(value.text);
					if (found == bindings_.end()) error(value.span, "unknown name '" + value.text + "'");
					value.type = found->second;
					break;
				}
				}
				return value.type;
			}

			// One module a `next` subformula speaks about: a layout id for `module(3)`, or the name of
			// an enclosing quantifier binding that will supply the id when the formula is grounded.
			struct StepSubject {
				uint16_t    id = 0;
				std::string binding;

				bool operator==(const StepSubject&) const = default;
			};

			// Collects the modules a `next` subformula speaks about. Bindings introduced inside that
			// subformula are rejected rather than collected: their module varies per grounding, so
			// they cannot name the single clock the operator advances on.
			void collect_step_subjects(const Value& value, std::vector<StepSubject>& subjects,
									   std::unordered_set<std::string>& inner_bindings, const Span& next_span) const {
				if (value.type == ValueType::MODULE) {
					if (value.kind == Value::Kind::NAME) {
						if (inner_bindings.contains(value.text)) {
							error(next_span, "next cannot resolve a module bound inside its own scope");
						}
						add_step_subject(StepSubject{0, value.text}, subjects, next_span);
					} else if (value.entity_id != 0) {
						add_step_subject(StepSubject{static_cast<uint16_t>(value.entity_id), {}}, subjects, next_span);
					}
				}
				if (value.base) collect_step_subjects(*value.base, subjects, inner_bindings, next_span);
				for (const auto& argument : value.arguments) {
					collect_step_subjects(*argument, subjects, inner_bindings, next_span);
				}
				if (value.domain) collect_step_subjects(*value.domain, subjects, inner_bindings, next_span);
				if (value.kind == Value::Kind::COUNT) {
					const bool fresh = inner_bindings.insert(value.binding).second;
					if (value.predicate) {
						collect_step_subjects(*value.predicate, subjects, inner_bindings, next_span);
					}
					if (fresh) inner_bindings.erase(value.binding);
				}
			}

			void collect_step_subjects(const Formula& formula, std::vector<StepSubject>& subjects,
									   std::unordered_set<std::string>& inner_bindings, const Span& next_span) const {
				if (formula.value) collect_step_subjects(*formula.value, subjects, inner_bindings, next_span);
				if (formula.compared) collect_step_subjects(*formula.compared, subjects, inner_bindings, next_span);
				if (formula.domain) collect_step_subjects(*formula.domain, subjects, inner_bindings, next_span);

				const bool quantifier = formula.kind == Formula::Kind::FORALL || formula.kind == Formula::Kind::EXISTS;
				const bool fresh      = quantifier && inner_bindings.insert(formula.binding).second;
				if (formula.left) collect_step_subjects(*formula.left, subjects, inner_bindings, next_span);
				if (formula.right) collect_step_subjects(*formula.right, subjects, inner_bindings, next_span);
				if (fresh) inner_bindings.erase(formula.binding);
			}

			void add_step_subject(StepSubject subject, std::vector<StepSubject>& subjects, const Span& span) const {
				for (const StepSubject& seen : subjects) {
					if (seen == subject) return;
				}
				if (!subjects.empty()) {
					error(span, "next refers to more than one module; parenthesize it around a single module");
				}
				subjects.push_back(std::move(subject));
			}

			void resolve_step_subject(Formula& formula) const {
				std::vector<StepSubject>        subjects;
				std::unordered_set<std::string> inner_bindings;
				collect_step_subjects(*formula.left, subjects, inner_bindings, formula.span);
				if (subjects.empty()) return;
				formula.step_module         = subjects.front().id;
				formula.step_module_binding = subjects.front().binding;
			}

			void check_formula(Formula& formula, bool count_predicate = false) {
				if (count_predicate &&
					(formula.kind == Formula::Kind::ALWAYS || formula.kind == Formula::Kind::EVENTUALLY ||
					 formula.kind == Formula::Kind::NEXT || formula.kind == Formula::Kind::UNTIL)) {
					error(formula.span, "count predicate cannot contain temporal operators");
				}
				if (formula.kind == Formula::Kind::ATOM) {
					const ValueType type = check(*formula.value);
					if (type != ValueType::BOOLEAN) {
						error(formula.value->span, "formula atom has type " + type_name(type) + ", expected Boolean");
					}
					return;
				}
				if (formula.kind == Formula::Kind::COMPARISON) {
					ValueType left  = check(*formula.value);
					ValueType right = check(*formula.compared, left);
					if (left == ValueType::UNKNOWN && right != ValueType::UNKNOWN) {
						left = check(*formula.value, right);
					}
					if (left == ValueType::UNKNOWN || right == ValueType::UNKNOWN) {
						error(formula.span, "enum literal type cannot be inferred uniquely");
					}
					if (left != right) {
						error(formula.span, "cannot compare " + type_name(left) + " with " + type_name(right));
					}
					validate_enum(*formula.value);
					validate_enum(*formula.compared);
					const bool ordered = formula.comparison != Formula::Comparison::EQUAL &&
										 formula.comparison != Formula::Comparison::NOT_EQUAL;
					if (ordered && left != ValueType::NUMBER) {
						error(formula.span, "ordered comparison requires numeric operands");
					}
					return;
				}
				if (formula.kind == Formula::Kind::FORALL || formula.kind == Formula::Kind::EXISTS) {
					if (count_predicate) error(formula.span, "count predicate cannot contain quantifiers");
					if (formula.domain) formula.binding_type = check_domain(*formula.domain);
					if (formula.binding_type != ValueType::MODULE && formula.binding_type != ValueType::TUB &&
						formula.binding_type != ValueType::RPC_REQUEST &&
						formula.binding_type != ValueType::RPC_RESPONSE &&
						formula.binding_type != ValueType::RPC_EVENT) {
						error(formula.span, "quantifier requires a finite typed domain");
					}
					enter_binding(formula.binding, formula.binding_type, formula.span);
					check_formula(*formula.left);
					bindings_.erase(formula.binding);
					return;
				}
				check_formula(*formula.left, count_predicate);
				if (formula.right) check_formula(*formula.right, count_predicate);
				// A sequencing `next` orders steps against the shared observation tick by construction.
				if (formula.kind == Formula::Kind::NEXT && !formula.sequencing) resolve_step_subject(formula);
			}

		public:
			Checker(std::string filename, const TypeEnvironment& environment)
				: filename_(std::move(filename)), modules_(environment.modules.begin(), environment.modules.end()) {}

			void run(File& file) {
				for (PropertyDeclaration& declaration : file.properties) {
					if (!names_.insert(declaration.name).second) {
						error(declaration.span, "duplicate declaration '" + declaration.name + "'");
					}
					check_formula(*declaration.formula);
				}
			}
		};

	} // namespace

	void type_check(File& file, const TypeEnvironment& environment, const std::string& filename) {
		Checker(filename, environment).run(file);
	}

} // namespace ltl
