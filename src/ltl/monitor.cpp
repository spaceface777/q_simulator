#include "monitor.h"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ltl {

	namespace {
		constexpr std::size_t MAINTENANCE_NODES    = 4096;
		constexpr std::size_t MAINTENANCE_FORMULAS = 256;

		enum class NodeKind : uint8_t {
			FALSE,
			TRUE,
			ATOM,
			NOT,
			AND,
			OR,
			ALWAYS,
			EVENTUALLY,
			NEXT,
			UNTIL,
			FORALL,
			EXISTS,
			DEFERRED,
		};

		struct Node;
		using NodePtr = std::shared_ptr<const Node>;

		// The outcome of the last progression of a node whose result did not depend on the tick: it
		// repeated itself or became a constant. Valid while the channels it read are unchanged and
		// before the next temporal window boundary inside it.
		struct StepCache {
			enum class Result : uint8_t { SELF, TRUE, FALSE };
			Result                result      = Result::SELF;
			uint64_t              input_tick  = 0;
			uint64_t              activations = 0;
			uint64_t              until       = 0;
			std::vector<uint32_t> reads;
		};

		struct Node {
			NodeKind       kind;
			const Formula* source;
			NodePtr        left;
			NodePtr        right;
			// Zero is a fresh temporal template. Otherwise this is its activation tick plus one.
			uint64_t                                      origin           = 0;
			mutable bool                                  modules_grounded = false;
			mutable bool                                  immutable_atom   = false;
			mutable std::vector<NodePtr>                  module_instances{};
			mutable std::unordered_map<uint64_t, NodePtr> tub_instances{};
			mutable NodePtr                               expansion{};
			mutable std::unique_ptr<StepCache>            step_cache{};
			// For a record quantifier: the shape of its body (-1 until known), and the static truth of
			// each body subformula under this node's ground bindings (-1 unknown, 0, 1).
			mutable int8_t                                     state_body = -1;
			mutable std::unordered_map<const Formula*, int8_t> static_facts{};
		};

		struct NodeKey {
			NodeKind       kind;
			const Formula* source;
			const Node*    left;
			const Node*    right;
			uint64_t       origin;

			bool operator==(const NodeKey&) const = default;
		};

		struct NodeKeyHash {
			std::size_t operator()(const NodeKey& key) const noexcept {
				std::size_t result  = static_cast<std::size_t>(key.kind);
				auto        combine = [&result](const void* value) {
					const std::size_t hash = std::hash<const void*>{}(value);
					result ^= hash + 0x9e3779b9 + (result << 6) + (result >> 2);
				};
				combine(key.source);
				combine(key.left);
				combine(key.right);
				result ^= std::hash<uint64_t>{}(key.origin) + 0x9e3779b9 + (result << 6) + (result >> 2);
				return result;
			}
		};

		bool immutable_value(const Value& value) {
			switch (value.kind) {
			case Value::Kind::BOOLEAN:
			case Value::Kind::NUMBER:
			case Value::Kind::COLOR:
			case Value::Kind::BYTES:
			case Value::Kind::ENUM_LITERAL: return true;
			case Value::Kind::NAME:         return value.entity_id != 0;
			case Value::Kind::COUNT:        return false;
			case Value::Kind::CALL:
				if (value.type == ValueType::MODULE || value.type == ValueType::TUB) return value.entity_id != 0;
				// A response reference has a fixed identity. Its status and payload do not.
				return value.type == ValueType::RPC_RESPONSE && immutable_value(*value.arguments[0]);
			case Value::Kind::MEMBER:
				if (!immutable_value(*value.base)) return false;
				if (value.base->type == ValueType::RPC_REQUEST || value.base->type == ValueType::RPC_EVENT) return true;
				if (value.base->type == ValueType::RPC_RESPONSE)
					return value.symbol == Value::Symbol::ID || value.symbol == Value::Symbol::REQUEST ||
						   value.symbol == Value::Symbol::MODULE || value.symbol == Value::Symbol::CMD;
				return value.symbol == Value::Symbol::LENGTH || value.symbol == Value::Symbol::ID ||
					   value.symbol == Value::Symbol::ENTRY || value.symbol == Value::Symbol::CENTER ||
					   value.symbol == Value::Symbol::LEFT_LASER || value.symbol == Value::Symbol::RIGHT_LASER;
			case Value::Kind::INDEX:
			case Value::Kind::SLICE:
			case Value::Kind::UNARY:
			case Value::Kind::BINARY:
				if (!immutable_value(*value.base)) return false;
				for (const auto& argument : value.arguments)
					if (!immutable_value(*argument)) return false;
				return true;
			}
			return false;
		}

		class NodeFactory {
		private:
			std::unordered_map<NodeKey, std::weak_ptr<const Node>, NodeKeyHash> nodes_;
			std::unordered_map<std::string, std::weak_ptr<const Node>>          atoms_;

			NodePtr intern(NodeKind kind, const Formula* source = nullptr, NodePtr left = {}, NodePtr right = {},
						   uint64_t origin = 0) {
				const NodeKey key{kind, source, left.get(), right.get(), origin};
				if (const auto found = nodes_.find(key); found != nodes_.end()) {
					if (NodePtr existing = found->second.lock()) return existing;
				}
				auto node   = std::make_shared<Node>(Node{kind, source, std::move(left), std::move(right), origin});
				nodes_[key] = node;
				return node;
			}

		public:
			NodeFactory() : false_(intern(NodeKind::FALSE)), true_(intern(NodeKind::TRUE)) {}

			void prune_expired() {
				for (auto iterator = nodes_.begin(); iterator != nodes_.end();) {
					if (iterator->second.expired()) iterator = nodes_.erase(iterator);
					else ++iterator;
				}
				for (auto iterator = atoms_.begin(); iterator != atoms_.end();) {
					if (iterator->second.expired()) iterator = atoms_.erase(iterator);
					else ++iterator;
				}
			}

			NodePtr constant(bool value) const { return value ? true_ : false_; }

			[[nodiscard]] std::size_t size() const noexcept { return nodes_.size() + atoms_.size(); }

			NodePtr atom(const Formula& source, const std::string& key) {
				if (const auto found = atoms_.find(key); found != atoms_.end()) {
					if (NodePtr existing = found->second.lock()) return existing;
				}
				NodePtr node = intern(NodeKind::ATOM, &source);
				node->immutable_atom =
					immutable_value(*source.value) && (!source.compared || immutable_value(*source.compared));
				atoms_[key] = node;
				return node;
			}

			NodePtr unary(NodeKind kind, const Formula& source, NodePtr child) {
				return intern(kind, &source, std::move(child));
			}

			NodePtr binary(NodeKind kind, const Formula& source, NodePtr left, NodePtr right) {
				return intern(kind, &source, std::move(left), std::move(right));
			}

			NodePtr advance(const NodePtr& node, uint64_t tick) {
				if (node->origin) return node;
				return intern(node->kind, node->source, node->left, node->right, tick + 1);
			}

			NodePtr make_not(NodePtr child, const Formula* source = nullptr) {
				if (child->kind == NodeKind::TRUE) return false_;
				if (child->kind == NodeKind::FALSE) return true_;
				if (child->kind == NodeKind::NOT) return child->left;
				return intern(NodeKind::NOT, source, std::move(child));
			}

			// The negation an implication normalizes to, kept even when it would fold away. Folding a
			// negated antecedent leaves no node carrying the implication, and the trigger count is read
			// off that node. Progression folds the constant a moment later, so nothing is lost but the
			// sharing of one node.
			NodePtr negate_antecedent(NodePtr child, const Formula* source) {
				return intern(NodeKind::NOT, source, std::move(child));
			}

			NodePtr make_and(NodePtr left, NodePtr right, const Formula* source = nullptr) {
				if (left->kind == NodeKind::FALSE || right->kind == NodeKind::FALSE) return false_;
				if (left->kind == NodeKind::TRUE) return right;
				if (right->kind == NodeKind::TRUE) return left;
				if (left == right) return left;
				if (right->kind == NodeKind::AND && (left == right->left || left == right->right)) return right;
				if (left->kind == NodeKind::AND && (right == left->left || right == left->right)) return left;
				return intern(NodeKind::AND, source, std::move(left), std::move(right));
			}

			NodePtr make_or(NodePtr left, NodePtr right, const Formula* source = nullptr) {
				if (left->kind == NodeKind::TRUE || right->kind == NodeKind::TRUE) return true_;
				if (left->kind == NodeKind::FALSE) return right;
				if (right->kind == NodeKind::FALSE) return left;
				if (left == right) return left;
				if (right->kind == NodeKind::OR && (left == right->left || left == right->right)) return right;
				if (left->kind == NodeKind::OR && (right == left->left || right == left->right)) return left;
				return intern(NodeKind::OR, source, std::move(left), std::move(right));
			}

		private:
			NodePtr false_;
			NodePtr true_;
		};

		std::string render_formula(const Formula& formula);

		const char* value_operator(Value::Operator operation) {
			switch (operation) {
			case Value::Operator::NEGATE:      return "-";
			case Value::Operator::BIT_NOT:     return "~";
			case Value::Operator::ADD:         return "+";
			case Value::Operator::SUBTRACT:    return "-";
			case Value::Operator::MULTIPLY:    return "*";
			case Value::Operator::DIVIDE:      return "/";
			case Value::Operator::MODULO:      return "%";
			case Value::Operator::SHIFT_LEFT:  return "<<";
			case Value::Operator::SHIFT_RIGHT: return ">>";
			case Value::Operator::BIT_AND:     return "&";
			case Value::Operator::BIT_XOR:     return "^";
			case Value::Operator::BIT_OR:      return "|";
			case Value::Operator::NONE:        break;
			}
			return "?";
		}

		void append_formula_key(std::ostringstream& output, const Formula& formula);

		void append_value_key(std::ostringstream& output, const Value& value) {
			output << static_cast<unsigned>(value.kind) << ':' << static_cast<unsigned>(value.type) << ':';
			switch (value.kind) {
			case Value::Kind::BOOLEAN: output << value.boolean; break;
			case Value::Kind::NUMBER:
				if (value.is_integer) output << value.integer;
				else output << std::setprecision(17) << value.number;
				break;
			case Value::Kind::COLOR: output << value.color; break;
			case Value::Kind::BYTES:
				output << value.bytes.size() << ':';
				for (const uint8_t byte : value.bytes) output << static_cast<unsigned>(byte) << ',';
				break;
			case Value::Kind::ENUM_LITERAL: output << '.' << value.text; break;
			case Value::Kind::COUNT:
				output << value.binding << ':' << static_cast<unsigned>(value.binding_type) << ':';
				if (value.domain) append_value_key(output, *value.domain);
				output << ':';
				append_formula_key(output, *value.predicate);
				break;
			case Value::Kind::NAME:
				output << value.entity_id;
				if (!value.entity_id) output << ':' << value.text;
				break;
			case Value::Kind::CALL:
				output << value.text << '(';
				for (const auto& argument : value.arguments) {
					append_value_key(output, *argument);
					output << ',';
				}
				output << ')';
				break;
			case Value::Kind::MEMBER:
				append_value_key(output, *value.base);
				output << '.' << value.text;
				break;
			case Value::Kind::INDEX:
				append_value_key(output, *value.base);
				output << '[';
				append_value_key(output, *value.arguments[0]);
				output << ']';
				break;
			case Value::Kind::SLICE:
				append_value_key(output, *value.base);
				output << '[';
				append_value_key(output, *value.arguments[0]);
				output << ':';
				append_value_key(output, *value.arguments[1]);
				output << ']';
				break;
			case Value::Kind::UNARY:
				output << value_operator(value.operation);
				append_value_key(output, *value.base);
				break;
			case Value::Kind::BINARY:
				output << '(';
				append_value_key(output, *value.base);
				output << value_operator(value.operation);
				append_value_key(output, *value.arguments[0]);
				output << ')';
				break;
			}
		}

		void append_formula_key(std::ostringstream& output, const Formula& formula) {
			output << '(' << static_cast<unsigned>(formula.kind) << ':' << static_cast<unsigned>(formula.comparison);
			if (formula.value) append_value_key(output, *formula.value);
			if (formula.compared) append_value_key(output, *formula.compared);
			if (formula.left) append_formula_key(output, *formula.left);
			if (formula.right) append_formula_key(output, *formula.right);
			output << ':' << formula.binding << ':' << static_cast<unsigned>(formula.binding_type);
			if (formula.domain) append_value_key(output, *formula.domain);
			output << ')';
		}

		std::string atom_key(const Formula& formula) {
			std::ostringstream output;
			output << static_cast<unsigned>(formula.kind) << ':' << static_cast<unsigned>(formula.comparison) << ':';
			append_value_key(output, *formula.value);
			if (formula.compared) {
				output << ':';
				append_value_key(output, *formula.compared);
			}
			return output.str();
		}

		std::string render_duration(uint64_t ticks) {
			if (ticks == 0) return "0ms";
			std::ostringstream output;
			const uint64_t     hours = ticks / 3600000;
			ticks %= 3600000;
			const uint64_t minutes = ticks / 60000;
			ticks %= 60000;
			const uint64_t seconds      = ticks / 1000;
			const uint64_t milliseconds = ticks % 1000;
			if (hours) output << hours << 'h';
			if (minutes) output << minutes << 'm';
			if (seconds) output << seconds << 's';
			if (milliseconds) output << milliseconds << "ms";
			return output.str();
		}

		std::string render_interval(const Formula& formula) {
			if (!formula.interval) return "";
			return "[" + render_duration(formula.interval->from_tick) + ", " +
				   render_duration(formula.interval->until_tick) + "]";
		}

		void render_value(std::ostringstream& output, const Value& value) {
			switch (value.kind) {
			case Value::Kind::BOOLEAN: output << (value.boolean ? "true" : "false"); break;
			case Value::Kind::NUMBER:
				if (value.is_integer) output << value.integer;
				else output << std::setprecision(17) << value.number;
				break;
			case Value::Kind::COLOR:
				output << "0x" << std::hex << std::setw(6) << std::setfill('0') << value.color << std::dec;
				break;
			case Value::Kind::BYTES:        output << value.text; break;
			case Value::Kind::ENUM_LITERAL: output << '.' << value.text; break;
			case Value::Kind::COUNT:
				output << "count " << value.binding;
				if (value.domain) {
					output << " in ";
					render_value(output, *value.domain);
				} else {
					static constexpr const char* types[] = {"Unknown",    "Boolean",   "Number",      "Color",
															"Module",     "Tub",       "Position",    "Bytes",
															"RpcCommand", "RpcStatus", "RpcEventKind"};
					output << ": " << types[static_cast<unsigned>(value.binding_type)];
				}
				output << " where (" << render_formula(*value.predicate) << ')';
				break;
			case Value::Kind::NAME:
				output << value.text;
				if (value.entity_id != 0 &&
					(value.type == ValueType::RPC_REQUEST || value.type == ValueType::RPC_RESPONSE ||
					 value.type == ValueType::RPC_EVENT)) {
					output << '#' << value.entity_id;
				}
				break;
			case Value::Kind::CALL:
				output << value.text << '(';
				for (std::size_t index = 0; index < value.arguments.size(); index++) {
					if (index != 0) output << ", ";
					render_value(output, *value.arguments[index]);
				}
				output << ')';
				break;
			case Value::Kind::MEMBER:
				render_value(output, *value.base);
				output << '.' << value.text;
				break;
			case Value::Kind::INDEX:
				render_value(output, *value.base);
				output << '[';
				render_value(output, *value.arguments[0]);
				output << ']';
				break;
			case Value::Kind::SLICE:
				render_value(output, *value.base);
				output << '[';
				render_value(output, *value.arguments[0]);
				output << ':';
				render_value(output, *value.arguments[1]);
				output << ']';
				break;
			case Value::Kind::UNARY:
				output << value_operator(value.operation);
				render_value(output, *value.base);
				break;
			case Value::Kind::BINARY:
				output << '(';
				render_value(output, *value.base);
				output << ' ' << value_operator(value.operation) << ' ';
				render_value(output, *value.arguments[0]);
				output << ')';
				break;
			}
		}

		std::string render_formula(const Formula& formula) {
			std::ostringstream output;
			if (formula.kind == Formula::Kind::ATOM) {
				render_value(output, *formula.value);
				return output.str();
			}
			if (formula.kind == Formula::Kind::COMPARISON) {
				render_value(output, *formula.value);
				static constexpr const char* operators[] = {"==", "!=", "<", "<=", ">", ">="};
				output << ' ' << operators[static_cast<unsigned>(formula.comparison)] << ' ';
				render_value(output, *formula.compared);
				return output.str();
			}
			if (formula.kind == Formula::Kind::NOT) return "!(" + render_formula(*formula.left) + ")";
			if (formula.kind == Formula::Kind::ALWAYS)
				return "always" + render_interval(formula) + " (" + render_formula(*formula.left) + ")";
			if (formula.kind == Formula::Kind::EVENTUALLY)
				return "eventually" + render_interval(formula) + " (" + render_formula(*formula.left) + ")";
			if (formula.kind == Formula::Kind::NEXT) return "next (" + render_formula(*formula.left) + ")";
			if (formula.kind == Formula::Kind::FORALL || formula.kind == Formula::Kind::EXISTS) {
				static constexpr const char* types[] = {"Unknown",    "Boolean",   "Number",      "Color",
														"Module",     "Tub",       "Position",    "Bytes",
														"RpcCommand", "RpcStatus", "RpcEventKind"};
				std::ostringstream           quantified;
				quantified << (formula.kind == Formula::Kind::FORALL ? "forall " : "exists ") << formula.binding;
				if (formula.domain) {
					quantified << " in ";
					render_value(quantified, *formula.domain);
				} else {
					quantified << ": " << types[static_cast<unsigned>(formula.binding_type)];
				}
				quantified << ": " << render_formula(*formula.left);
				return quantified.str();
			}
			const char* operation = nullptr;
			switch (formula.kind) {
			case Formula::Kind::AND:     operation = "&&"; break;
			case Formula::Kind::OR:      operation = "||"; break;
			case Formula::Kind::IMPLIES: operation = "->"; break;
			case Formula::Kind::UNTIL:   operation = "until"; break;
			default:                     throw std::logic_error("cannot render unknown LTL formula kind");
			}
			return "(" + render_formula(*formula.left) + " " + operation + render_interval(formula) + " " +
				   render_formula(*formula.right) + ")";
		}

		std::string render_node(const NodePtr& node) {
			switch (node->kind) {
			case NodeKind::FALSE: return "false";
			case NodeKind::TRUE:  return "true";
			case NodeKind::ATOM:  return render_formula(*node->source);
			case NodeKind::NOT:   return "!(" + render_node(node->left) + ")";
			case NodeKind::ALWAYS:
				return "always" + render_interval(*node->source) + " (" + render_node(node->left) + ")";
			case NodeKind::EVENTUALLY:
				return "eventually" + render_interval(*node->source) + " (" + render_node(node->left) + ")";
			case NodeKind::NEXT: return "next (" + render_node(node->left) + ")";
			case NodeKind::AND:  return "(" + render_node(node->left) + " && " + render_node(node->right) + ")";
			case NodeKind::OR:   return "(" + render_node(node->left) + " || " + render_node(node->right) + ")";
			case NodeKind::UNTIL:
				return "(" + render_node(node->left) + " until" + render_interval(*node->source) + " " +
					   render_node(node->right) + ")";
			case NodeKind::FORALL:
			case NodeKind::EXISTS: return render_formula(*node->source);
			case NodeKind::DEFERRED:
				return node->expansion ? render_node(node->expansion) : render_formula(*node->source->right);
			}
			throw std::logic_error("unknown normalized LTL node kind");
		}

		NodePtr normalize(const Formula& formula, NodeFactory& factory) {
			switch (formula.kind) {
			case Formula::Kind::ATOM:
				if (formula.value->kind == Value::Kind::BOOLEAN) return factory.constant(formula.value->boolean);
				return factory.atom(formula, atom_key(formula));
			case Formula::Kind::COMPARISON: return factory.atom(formula, atom_key(formula));
			case Formula::Kind::NOT:        return factory.make_not(normalize(*formula.left, factory), &formula);
			case Formula::Kind::ALWAYS:
				return factory.unary(NodeKind::ALWAYS, formula, normalize(*formula.left, factory));
			case Formula::Kind::EVENTUALLY:
				return factory.unary(NodeKind::EVENTUALLY, formula, normalize(*formula.left, factory));
			case Formula::Kind::NEXT: return factory.unary(NodeKind::NEXT, formula, normalize(*formula.left, factory));
			case Formula::Kind::AND:
				return factory.make_and(normalize(*formula.left, factory),
										formula.ground_bindings.empty()
											? normalize(*formula.right, factory)
											: factory.unary(NodeKind::DEFERRED, formula, {}),
										&formula);
			case Formula::Kind::OR:
				return factory.make_or(normalize(*formula.left, factory),
									   formula.ground_bindings.empty() ? normalize(*formula.right, factory)
																	   : factory.unary(NodeKind::DEFERRED, formula, {}),
									   &formula);
			case Formula::Kind::IMPLIES:
				return factory.make_or(factory.negate_antecedent(normalize(*formula.left, factory), &formula),
									   factory.unary(NodeKind::DEFERRED, formula, {}), &formula);
			case Formula::Kind::UNTIL:
				return factory.binary(NodeKind::UNTIL, formula, normalize(*formula.left, factory),
									  normalize(*formula.right, factory));
			case Formula::Kind::FORALL: return factory.unary(NodeKind::FORALL, formula, {});
			case Formula::Kind::EXISTS: return factory.unary(NodeKind::EXISTS, formula, {});
			}
			throw std::logic_error("unknown LTL formula kind");
		}

		std::unique_ptr<Value> clone_value(const Value&                                         value,
										   const std::vector<std::pair<std::string, uint64_t>>& bindings);

		// Only fold facts independent of observations. In particular, a false dynamic guard
		// must never erase a future obligation.
		std::optional<bool> static_truth(const Formula&                                       formula,
										 const std::vector<std::pair<std::string, uint64_t>>& bindings) {
			auto entity = [&](const Value& value) -> uint64_t {
				if (value.type != ValueType::MODULE && value.type != ValueType::TUB) return 0;
				if (value.kind == Value::Kind::NAME) {
					for (const auto& [binding, identity] : bindings)
						if (value.text == binding) return identity;
					return value.entity_id;
				}
				if (value.kind == Value::Kind::CALL && (value.text == "module" || value.text == "tub"))
					return value.entity_id;
				return 0;
			};
			if (formula.kind == Formula::Kind::ATOM && formula.value->kind == Value::Kind::BOOLEAN)
				return formula.value->boolean;
			if (formula.kind == Formula::Kind::COMPARISON) {
				const auto a = entity(*formula.value), b = entity(*formula.compared);
				if (a && b) {
					if (formula.comparison == Formula::Comparison::EQUAL) return a == b;
					if (formula.comparison == Formula::Comparison::NOT_EQUAL) return a != b;
				}
			}
			if (formula.kind == Formula::Kind::NOT) {
				if (auto value = static_truth(*formula.left, bindings)) return !*value;
			}
			if (formula.kind == Formula::Kind::FORALL || formula.kind == Formula::Kind::EXISTS) {
				const bool universal = formula.kind == Formula::Kind::FORALL;
				if (auto value = static_truth(*formula.left, bindings); value && *value == universal) return universal;
			}
			if (formula.kind == Formula::Kind::IMPLIES) {
				if (auto a = static_truth(*formula.left, bindings); a && !*a) return true;
				return std::nullopt;
			}
			if (formula.kind == Formula::Kind::AND || formula.kind == Formula::Kind::OR) {
				auto       a           = static_truth(*formula.left, bindings);
				const bool conjunction = formula.kind == Formula::Kind::AND;
				if (a && *a != conjunction) return *a;
				auto b = static_truth(*formula.right, bindings);
				if (b && *b != conjunction) return *b;
				if (a && b) return conjunction;
			}
			return std::nullopt;
		}

		std::unique_ptr<Formula> clone_formula(const Formula&                                       formula,
											   const std::vector<std::pair<std::string, uint64_t>>& bindings,
											   bool                                                 defer = true) {
			auto result = std::make_unique<Formula>();
			if (auto truth = static_truth(formula, bindings)) {
				result->value          = std::make_unique<Value>();
				result->value->kind    = Value::Kind::BOOLEAN;
				result->value->type    = ValueType::BOOLEAN;
				result->value->boolean = *truth;
				return result;
			}
			result->kind         = formula.kind;
			result->span         = formula.span;
			result->interval     = formula.interval;
			result->comparison   = formula.comparison;
			result->binding      = formula.binding;
			result->binding_type = formula.binding_type;

			// A `next` whose module came from this quantifier learns its id as the formula grounds.
			result->step_module         = formula.step_module;
			result->step_module_binding = formula.step_module_binding;
			result->sequencing          = formula.sequencing;
			for (const auto& [binding, identity] : bindings)
				if (!formula.step_module_binding.empty() && formula.step_module_binding == binding) {
					result->step_module = static_cast<uint16_t>(identity);
					result->step_module_binding.clear();
				}
			if (formula.value) result->value = clone_value(*formula.value, bindings);
			if (formula.compared) result->compared = clone_value(*formula.compared, bindings);
			if (defer && (formula.kind == Formula::Kind::FORALL || formula.kind == Formula::Kind::EXISTS)) {
				result->left            = formula.left;
				result->ground_bindings = formula.ground_bindings;
				result->ground_bindings.insert(result->ground_bindings.end(), bindings.begin(), bindings.end());
			} else if (formula.left) result->left = clone_formula(*formula.left, bindings, defer);
			if (defer &&
				(formula.kind == Formula::Kind::IMPLIES ||
				 (!bindings.empty() && (formula.kind == Formula::Kind::AND || formula.kind == Formula::Kind::OR)))) {
				result->right           = formula.right;
				result->ground_bindings = formula.ground_bindings;
				result->ground_bindings.insert(result->ground_bindings.end(), bindings.begin(), bindings.end());
			} else if (formula.right) result->right = clone_formula(*formula.right, bindings, defer);
			if (formula.domain) result->domain = clone_value(*formula.domain, bindings);
			return result;
		}

		std::unique_ptr<Value> clone_value(const Value&                                         value,
										   const std::vector<std::pair<std::string, uint64_t>>& bindings) {
			auto result          = std::make_unique<Value>();
			result->kind         = value.kind;
			result->span         = value.span;
			result->type         = value.type;
			result->text         = value.text;
			result->number       = value.number;
			result->integer      = value.integer;
			result->is_integer   = value.is_integer;
			result->color        = value.color;
			result->bytes        = value.bytes;
			result->entity_id    = value.entity_id;
			result->enum_value   = value.enum_value;
			result->symbol       = value.symbol;
			result->operation    = value.operation;
			result->boolean      = value.boolean;
			result->binding      = value.binding;
			result->binding_type = value.binding_type;
			for (const auto& [binding, identity] : bindings)
				if (value.kind == Value::Kind::NAME && value.text == binding) {
					result->entity_id = identity;
				}
			if (value.base) result->base = clone_value(*value.base, bindings);
			for (const auto& argument : value.arguments) {
				result->arguments.push_back(clone_value(*argument, bindings));
			}
			if (value.domain) result->domain = clone_value(*value.domain, bindings);
			if (value.predicate) result->predicate = clone_formula(*value.predicate, bindings, false);
			return result;
		}

		enum BodyShape : int8_t {
			TEMPORAL_BODY = 0,
			// No temporal operator or quantifier: the body evaluates to a constant.
			STATE_BODY = 1,
			// `state -> anything`: a false antecedent decides the body without grounding it.
			GUARDED_BODY = 2,
		};

		bool state_only(const Formula& formula);

		int8_t body_shape(const Formula& body) {
			if (state_only(body)) return STATE_BODY;
			if (body.kind == Formula::Kind::IMPLIES && state_only(*body.left)) return GUARDED_BODY;
			return TEMPORAL_BODY;
		}

		bool state_only(const Formula& formula) {
			switch (formula.kind) {
			case Formula::Kind::ATOM:
			case Formula::Kind::COMPARISON: return true;
			case Formula::Kind::NOT:        return state_only(*formula.left);
			case Formula::Kind::AND:
			case Formula::Kind::OR:
			case Formula::Kind::IMPLIES:    return state_only(*formula.left) && state_only(*formula.right);
			default:                        return false;
			}
		}

		// Evaluates a state-only body under bindings with exactly the outcome of grounding it with
		// clone_formula, normalizing, and progressing it: the same static folding at every level, the
		// same left-to-right short circuits, and the same trigger count for implications.
		bool evaluate_state(const Formula& formula, const std::vector<std::pair<std::string, uint64_t>>& bindings,
							const Node& quantifier, const InputTracker& inputs, uint64_t& activations) {
			auto [fact, inserted] = quantifier.static_facts.try_emplace(&formula, int8_t{-1});
			if (inserted) {
				// Record bindings never take part in static truth, which only compares entities.
				if (const auto truth = static_truth(formula, bindings)) fact->second = *truth ? 1 : 0;
			}
			if (fact->second >= 0) return fact->second == 1;
			switch (formula.kind) {
			case Formula::Kind::ATOM:
			case Formula::Kind::COMPARISON: return inputs.evaluate_bound(formula, bindings);
			case Formula::Kind::NOT: return !evaluate_state(*formula.left, bindings, quantifier, inputs, activations);
			case Formula::Kind::AND:
				return evaluate_state(*formula.left, bindings, quantifier, inputs, activations) &&
					   evaluate_state(*formula.right, bindings, quantifier, inputs, activations);
			case Formula::Kind::OR:
				return evaluate_state(*formula.left, bindings, quantifier, inputs, activations) ||
					   evaluate_state(*formula.right, bindings, quantifier, inputs, activations);
			case Formula::Kind::IMPLIES: {
				const bool antecedent = evaluate_state(*formula.left, bindings, quantifier, inputs, activations);
				if (!antecedent) return true;
				activations++;
				return evaluate_state(*formula.right, bindings, quantifier, inputs, activations);
			}
			default: throw std::logic_error("state evaluation reached a temporal formula");
			}
		}

		using FormulaOwner       = std::function<void(std::unique_ptr<Formula>, bool)>;
		using PersistentGrounder = std::function<NodePtr(const Formula&, const std::string&, uint64_t)>;

		NodePtr progress(const NodePtr& node, const AtomEvaluator& evaluate, NodeFactory& factory,
						 const DomainEvaluator& evaluate_domain, const FormulaOwner& own_formula,
						 const PersistentGrounder& ground_persistent, const StepEvaluator& evaluate_step,
						 uint64_t& activations, uint64_t tick, uint64_t& wakeup,
						 std::vector<std::pair<const Formula*, bool>>& evaluations, const InputTracker* inputs);

		// Progresses an independent obligation, reusing its last outcome when that outcome repeats.
		NodePtr progress_cached(const NodePtr& node, const AtomEvaluator& evaluate, NodeFactory& factory,
								const DomainEvaluator& evaluate_domain, const FormulaOwner& own_formula,
								const PersistentGrounder& ground_persistent, const StepEvaluator& evaluate_step,
								uint64_t& activations, uint64_t tick, uint64_t& wakeup,
								std::vector<std::pair<const Formula*, bool>>& evaluations, const InputTracker* inputs) {
			if (!inputs || !inputs->reads || !inputs->changed_since)
				return progress(node, evaluate, factory, evaluate_domain, own_formula, ground_persistent, evaluate_step,
								activations, tick, wakeup, evaluations, inputs);
			std::vector<uint32_t>& reads = *inputs->reads;
			if (const auto& cache = node->step_cache;
				cache && tick < cache->until && !inputs->changed_since(cache->reads, cache->input_tick)) {
				activations += cache->activations;
				wakeup = std::min(wakeup, cache->until);
				reads.insert(reads.end(), cache->reads.begin(), cache->reads.end());
				if (cache->result == StepCache::Result::SELF) return node;
				return factory.constant(cache->result == StepCache::Result::TRUE);
			}
			const std::size_t start        = reads.size();
			const uint64_t    outer_wakeup = wakeup;
			const uint64_t    before       = activations;
			wakeup                         = std::numeric_limits<uint64_t>::max();
			NodePtr        result = progress(node, evaluate, factory, evaluate_domain, own_formula, ground_persistent,
											 evaluate_step, activations, tick, wakeup, evaluations, inputs);
			const uint64_t local_wakeup = wakeup;
			wakeup                      = std::min(outer_wakeup, local_wakeup);
			const bool repeatable = result == node || result->kind == NodeKind::TRUE || result->kind == NodeKind::FALSE;
			if (!repeatable) {
				node->step_cache.reset();
				return result;
			}
			if (!node->step_cache) node->step_cache = std::make_unique<StepCache>();
			StepCache& cache  = *node->step_cache;
			cache.result      = result == node                   ? StepCache::Result::SELF
								: result->kind == NodeKind::TRUE ? StepCache::Result::TRUE
																 : StepCache::Result::FALSE;
			cache.input_tick  = inputs->tick;
			cache.activations = activations - before;
			cache.until       = local_wakeup;
			cache.reads.assign(reads.begin() + static_cast<std::ptrdiff_t>(start), reads.end());
			if (inputs->deduplicate) inputs->deduplicate(cache.reads);
			else {
				std::ranges::sort(cache.reads);
				const auto duplicates = std::ranges::unique(cache.reads);
				cache.reads.erase(duplicates.begin(), duplicates.end());
			}
			return result;
		}

		NodePtr progress(const NodePtr& node, const AtomEvaluator& evaluate, NodeFactory& factory,
						 const DomainEvaluator& evaluate_domain, const FormulaOwner& own_formula,
						 const PersistentGrounder& ground_persistent, const StepEvaluator& evaluate_step,
						 uint64_t& activations, uint64_t tick, uint64_t& wakeup,
						 std::vector<std::pair<const Formula*, bool>>& evaluations, const InputTracker* inputs) {
			const uint64_t elapsed = node->origin ? tick - (node->origin - 1) : 0;
			// Repeating a self-loop is safe only until the next temporal window boundary.
			if ((node->kind == NodeKind::ALWAYS || node->kind == NodeKind::EVENTUALLY ||
				 node->kind == NodeKind::UNTIL) &&
				node->source->interval && elapsed < node->source->interval->until_tick) {
				const auto&    interval = *node->source->interval;
				const uint64_t delay =
					(elapsed < interval.from_tick ? interval.from_tick : interval.until_tick) - elapsed;
				if (delay < std::numeric_limits<uint64_t>::max() - tick) wakeup = std::min(wakeup, tick + delay);
			}
			if (node->kind == NodeKind::NEXT && elapsed == 0) wakeup = std::min(wakeup, tick + 1);
			auto progress_child = [&](const NodePtr& child) {
				return progress(child, evaluate, factory, evaluate_domain, own_formula, ground_persistent,
								evaluate_step, activations, tick, wakeup, evaluations, inputs);
			};
			auto progress_instance = [&](const NodePtr& child) {
				return progress_cached(child, evaluate, factory, evaluate_domain, own_formula, ground_persistent,
									   evaluate_step, activations, tick, wakeup, evaluations, inputs);
			};
			switch (node->kind) {
			case NodeKind::FALSE: return factory.constant(false);
			case NodeKind::TRUE:  return factory.constant(true);
			case NodeKind::ATOM:  {
				// Cache only immutable, grounded data, and only after normal short-circuit evaluation
				// reaches the atom. Speculative evaluation could access an invalid payload index.
				NodePtr result = node->immutable_atom ? node->expansion : NodePtr{};
				if (!result) {
					result = factory.constant(evaluate(*node->source));
					if (node->immutable_atom) node->expansion = result;
				}
				evaluations.emplace_back(node->source, result->kind == NodeKind::TRUE);
				return result;
			}
			case NodeKind::NOT: {
				NodePtr inner = progress_child(node->left);
				// An implication normalizes to `!antecedent || consequent`, so this negation standing
				// over a true antecedent is the moment the property imposed a real obligation. Counting
				// it separates a pass that tested something from a pass that never fired.
				if (inner->kind == NodeKind::TRUE && node->source && node->source->kind == Formula::Kind::IMPLIES) {
					activations++;
				}
				return factory.make_not(std::move(inner), node->source);
			}
			case NodeKind::AND: {
				NodePtr left = progress_child(node->left);
				if (left->kind == NodeKind::FALSE) return left;
				return factory.make_and(std::move(left), progress_child(node->right), node->source);
			}
			case NodeKind::OR: {
				NodePtr left = progress_child(node->left);
				if (left->kind == NodeKind::TRUE) return left;
				return factory.make_or(std::move(left), progress_child(node->right), node->source);
			}
			case NodeKind::ALWAYS:
				if (!node->source->interval) {
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::FALSE) return current;
					return factory.make_and(std::move(current), node, node->source);
				}
				if (elapsed < node->source->interval->from_tick) return factory.advance(node, tick);
				if (elapsed == node->source->interval->until_tick) {
					return progress_child(node->left);
				}
				{
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::FALSE) return current;
					return factory.make_and(std::move(current), factory.advance(node, tick), node->source);
				}
			case NodeKind::EVENTUALLY:
				if (!node->source->interval) {
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::TRUE) return current;
					return factory.make_or(std::move(current), node, node->source);
				}
				if (elapsed < node->source->interval->from_tick) return factory.advance(node, tick);
				if (elapsed == node->source->interval->until_tick) {
					return progress_child(node->left);
				}
				{
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::TRUE) return current;
					return factory.make_or(std::move(current), factory.advance(node, tick), node->source);
				}
			case NodeKind::NEXT:
				// The observation the obligation was created on is the present, never the next step.
				if (elapsed == 0) return factory.advance(node, tick);
				// A module that was not granted this tick has not advanced, so its next step is still
				// ahead. Hold the obligation rather than discharging it against a tick whose code did
				// not run. Module zero is the global tick, which advances on every observation.
				if (node->source->step_module != 0 && evaluate_step && !evaluate_step(node->source->step_module)) {
					return node;
				}
				return progress_child(node->left);
			case NodeKind::UNTIL:
				if (!node->source->interval) {
					NodePtr witness = progress_child(node->right);
					if (witness->kind == NodeKind::TRUE) return witness;
					return factory.make_or(std::move(witness),
										   factory.make_and(progress_child(node->left), node, node->source),
										   node->source);
				}
				if (elapsed == node->source->interval->until_tick) {
					return progress_child(node->right);
				}
				if (elapsed < node->source->interval->from_tick) {
					NodePtr prefix = progress_child(node->left);
					if (prefix->kind == NodeKind::FALSE) return prefix;
					return factory.make_and(std::move(prefix), factory.advance(node, tick), node->source);
				}
				{
					NodePtr witness = progress_child(node->right);
					if (witness->kind == NodeKind::TRUE) return witness;
					NodePtr prefix = progress_child(node->left);
					NodePtr continuation =
						prefix->kind == NodeKind::FALSE
							? prefix
							: factory.make_and(std::move(prefix), factory.advance(node, tick), node->source);
					return factory.make_or(std::move(witness), std::move(continuation), node->source);
				}
			case NodeKind::DEFERRED: {
				if (!node->expansion) node->expansion = ground_persistent(*node->source, {}, 0);
				return progress_child(node->expansion);
			}
			case NodeKind::FORALL:
			case NodeKind::EXISTS: {
				if (!evaluate_domain) throw std::logic_error("quantified monitor requires a domain evaluator");
				const bool universal = node->kind == NodeKind::FORALL;
				NodePtr    result    = factory.constant(universal);
				if (!node->source->domain && node->source->binding_type == ValueType::MODULE) {
					if (!node->modules_grounded) {
						evaluate_domain(nullptr, ValueType::MODULE, [&](uint64_t identity) {
							NodePtr instance = ground_persistent(*node->source, node->source->binding, identity);
							if (instance->kind != (universal ? NodeKind::TRUE : NodeKind::FALSE))
								node->module_instances.push_back(std::move(instance));
						});
						node->modules_grounded = true;
					}
					for (const auto& instance : node->module_instances) {
						NodePtr item = progress_instance(instance);
						result       = universal ? factory.make_and(std::move(result), std::move(item), node->source)
												 : factory.make_or(std::move(result), std::move(item), node->source);
						if (result->kind == (universal ? NodeKind::FALSE : NodeKind::TRUE)) break;
					}
					return result;
				}
				evaluate_domain(node->source->domain.get(), node->source->binding_type, [&](uint64_t identity) {
					if ((universal && result->kind == NodeKind::FALSE) ||
						(!universal && result->kind == NodeKind::TRUE))
						return;
					NodePtr item;
					if (!node->source->domain) {
						auto& instance = node->tub_instances[identity];
						if (!instance) instance = ground_persistent(*node->source, node->source->binding, identity);
						item = progress_instance(instance);
					} else {
						auto bindings = node->source->ground_bindings;
						bindings.emplace_back(node->source->binding, identity);
						const Formula& body = *node->source->left;
						if (inputs && inputs->evaluate_bound) {
							if (node->state_body < 0) node->state_body = body_shape(body);
							if (node->state_body == STATE_BODY) {
								item = factory.constant(evaluate_state(body, bindings, *node, *inputs, activations));
							} else if (node->state_body == GUARDED_BODY) {
								// Grounding would count the antecedent's own implications while progressing it.
								// When the antecedent holds, the grounded body counts them again, so drop these.
								uint64_t antecedent_activations = 0;
								if (!evaluate_state(*body.left, bindings, *node, *inputs, antecedent_activations)) {
									activations += antecedent_activations;
									item = factory.constant(true);
								}
							}
						}
						if (!item) {
							auto grounded         = clone_formula(body, bindings);
							item                  = progress_child(normalize(*grounded, factory));
							const bool persistent = item->kind != NodeKind::TRUE && item->kind != NodeKind::FALSE;
							own_formula(std::move(grounded), persistent);
						}
					}
					result = universal ? factory.make_and(std::move(result), std::move(item), node->source)
									   : factory.make_or(std::move(result), std::move(item), node->source);
				});
				return result;
			}
			}
			throw std::logic_error("unknown normalized LTL formula kind");
		}

		// Applies finite-trace end semantics. An open eventuality is unmet, so the pessimistic reading
		// refutes it; the optimistic reading grants it, standing for the longer trace that could have
		// satisfied it. Where the two disagree the trace simply ended too early to decide, which is
		// what separates an unresolved property from a failed one. A deadline that expired inside the
		// trace never reaches here: it was already decided while progressing.
		NodePtr finish(const NodePtr& node, NodeFactory& factory, bool optimistic = false) {
			switch (node->kind) {
			case NodeKind::FALSE: return factory.constant(false);
			case NodeKind::TRUE:  return factory.constant(true);
			case NodeKind::ATOM:  throw std::logic_error("unprogressed atom at finite-trace end");
			case NodeKind::NOT:   return factory.make_not(finish(node->left, factory, optimistic), node->source);
			case NodeKind::AND:   {
				NodePtr left = finish(node->left, factory, optimistic);
				if (left->kind == NodeKind::FALSE) return left;
				return factory.make_and(std::move(left), finish(node->right, factory, optimistic), node->source);
			}
			case NodeKind::OR: {
				NodePtr left = finish(node->left, factory, optimistic);
				if (left->kind == NodeKind::TRUE) return left;
				return factory.make_or(std::move(left), finish(node->right, factory, optimistic), node->source);
			}
			case NodeKind::ALWAYS:     return factory.constant(true);
			case NodeKind::EVENTUALLY: return factory.constant(optimistic);
			case NodeKind::NEXT:       return factory.constant(optimistic);
			case NodeKind::UNTIL:      return factory.constant(optimistic);
			case NodeKind::FORALL:
			case NodeKind::EXISTS:
			case NodeKind::DEFERRED:   throw std::logic_error("unprogressed quantifier at finite-trace end");
			}
			throw std::logic_error("unknown normalized LTL formula kind");
		}

		// Drops conjuncts of the top-level conjunction that another conjunct implies. Copies of one
		// timed obligation differ only by their start. Once both copies are inside their windows, an
		// eventuality or until with the earlier deadline implies the later one, and an always with the
		// later deadline implies the earlier one. The kept copy decides on the same tick as the pair, so
		// verdicts and their ticks are unchanged, while an obligation triggered on every tick of a
		// window no longer accumulates one copy per tick.
		NodePtr subsume(const NodePtr& residual, NodeFactory& factory, uint64_t tick) {
			if (residual->kind != NodeKind::AND) return residual;
			std::vector<NodePtr> leaves;
			std::vector<NodePtr> stack{residual};
			while (!stack.empty()) {
				NodePtr node = std::move(stack.back());
				stack.pop_back();
				if (node->kind == NodeKind::AND) {
					stack.push_back(node->right);
					stack.push_back(node->left);
				} else leaves.push_back(std::move(node));
			}

			struct Key {
				NodeKind       kind;
				const Formula* source;
				const Node*    left;
				const Node*    right;
				bool           operator==(const Key&) const = default;
			};

			struct KeyHash {
				std::size_t operator()(const Key& key) const noexcept {
					return std::hash<const void*>{}(key.source) ^ (std::hash<const void*>{}(key.left) << 1) ^
						   (std::hash<const void*>{}(key.right) << 2) ^ static_cast<std::size_t>(key.kind);
				}
			};

			// The next progression sees elapsed = tick + 1 - (origin - 1).
			const auto started = [tick](const NodePtr& leaf) {
				return (leaf->kind == NodeKind::ALWAYS || leaf->kind == NodeKind::EVENTUALLY ||
						leaf->kind == NodeKind::UNTIL) &&
					   leaf->origin && leaf->source->interval &&
					   tick + 2 - leaf->origin >= leaf->source->interval->from_tick;
			};
			if (std::ranges::count_if(leaves, started) < 2) return residual;

			std::unordered_map<Key, std::size_t, KeyHash> timed;
			std::vector<NodePtr>                          kept;
			kept.reserve(leaves.size());
			bool dropped = false;
			for (NodePtr& leaf : leaves) {
				if (started(leaf)) {
					const Key key{leaf->kind, leaf->source, leaf->left.get(), leaf->right.get()};
					const auto [found, inserted] = timed.try_emplace(key, kept.size());
					if (!inserted) {
						dropped               = true;
						NodePtr&   other      = kept[found->second];
						const bool keep_later = leaf->kind == NodeKind::ALWAYS;
						if (keep_later ? leaf->origin > other->origin : leaf->origin < other->origin)
							other = std::move(leaf);
						continue;
					}
				}
				kept.push_back(std::move(leaf));
			}
			if (!dropped) return residual;
			NodePtr result = kept.back();
			for (std::size_t index = kept.size() - 1; index-- > 0;)
				result = factory.make_and(kept[index], std::move(result));
			return result;
		}

		Verdict verdict(const NodePtr& residual) noexcept {
			if (residual->kind == NodeKind::TRUE) return Verdict::TRUE;
			if (residual->kind == NodeKind::FALSE) return Verdict::FALSE;
			return Verdict::PENDING;
		}

		struct TimedObligation {
			const Formula* formula = nullptr;
			uint64_t       elapsed = 0;
		};

		void find_timed_obligation(const NodePtr& node, TimedObligation& result,
								   std::unordered_set<const Node*>& visited, uint64_t tick) {
			if (!visited.insert(node.get()).second) return;
			const uint64_t elapsed = node->origin ? tick - (node->origin - 1) : 0;
			if (node->source && node->source->interval && (!result.formula || elapsed > result.elapsed)) {
				result = TimedObligation{node->source, elapsed};
			}
			if (node->left) find_timed_obligation(node->left, result, visited, tick);
			if (node->right) find_timed_obligation(node->right, result, visited, tick);
		}

		void collect_formula_sources(const NodePtr& node, std::unordered_set<const Formula*>& sources,
									 std::unordered_set<const Node*>& visited) {
			if (!node || !visited.insert(node.get()).second) return;
			if (node->source) sources.insert(node->source);
			collect_formula_sources(node->left, sources, visited);
			collect_formula_sources(node->right, sources, visited);
			collect_formula_sources(node->expansion, sources, visited);
			for (const auto& instance : node->module_instances) collect_formula_sources(instance, sources, visited);
			for (const auto& [id, instance] : node->tub_instances) collect_formula_sources(instance, sources, visited);
		}

		bool formula_tree_is_referenced(const Formula& formula, const std::unordered_set<const Formula*>& sources) {
			if (sources.contains(&formula)) return true;
			// Borrowed bodies are not owned by this grounding. Their normalized expansions
			// have separate owners. Searching the original macro tree here is both costly
			// and liable to retain a dead grounding because of an unrelated live source.
			const bool deferred_left  = formula.kind == Formula::Kind::FORALL || formula.kind == Formula::Kind::EXISTS;
			const bool deferred_right = formula.kind == Formula::Kind::IMPLIES ||
										(!formula.ground_bindings.empty() &&
										 (formula.kind == Formula::Kind::AND || formula.kind == Formula::Kind::OR));
			return (!deferred_left && formula.left && formula_tree_is_referenced(*formula.left, sources)) ||
				   (!deferred_right && formula.right && formula_tree_is_referenced(*formula.right, sources));
		}

	} // namespace

	namespace {
		// Whether the property states an obligation conditional on something happening. Only these
		// can pass without ever having been tested, so only these carry a meaningful trigger count.
		bool contains_implication(const Formula& formula) {
			if (formula.kind == Formula::Kind::IMPLIES) return true;
			if (formula.left && contains_implication(*formula.left)) return true;
			if (formula.right && contains_implication(*formula.right)) return true;
			return false;
		}
	} // namespace

	class Monitor::Implementation {
	public:
		Implementation(std::string property_name, const Formula& formula, uint64_t activation_tick)
			: property_name(std::move(property_name)), source_formula(formula), residual(normalize(formula, factory)),
			  trigger(contains_implication(formula)), activation_tick(activation_tick) {}

		std::string                                  property_name;
		const Formula&                               source_formula;
		mutable std::string                          formula;
		NodeFactory                                  factory;
		NodePtr                                      residual;
		std::vector<std::pair<const Formula*, bool>> last_evaluations;
		std::vector<std::unique_ptr<Formula>>        transient_formulas;
		std::vector<std::unique_ptr<Formula>>        persistent_formulas;
		std::optional<FailureExplanation>            failure;
		std::string                                  obligation_at_end;
		bool                                         observed            = false;
		bool                                         finished            = false;
		bool                                         unresolved          = false;
		bool                                         trigger             = false;
		uint64_t                                     activation_tick     = 0;
		uint64_t                                     observations        = 0;
		uint64_t                                     activations         = 0;
		uint64_t                                     last_revision       = 0;
		uint64_t                                     stutter_activations = 0;
		bool                                         stutter             = false;
		bool                                         progressed          = false;
		uint64_t                                     wakeup              = 0;
		// Maintenance runs when the interned nodes or the ground formulas have doubled since the last
		// sweep, which keeps its cost proportional to the growth it reclaims.
		std::size_t next_node_sweep    = MAINTENANCE_NODES;
		std::size_t next_formula_sweep = MAINTENANCE_FORMULAS;
	};

	Monitor::Monitor(std::string property_name, const Formula& formula, uint64_t activation_tick)
		: implementation_(std::make_unique<Implementation>(std::move(property_name), formula, activation_tick)) {}

	Monitor::~Monitor() = default;

	Monitor::Monitor(Monitor&&) noexcept = default;

	Monitor& Monitor::operator=(Monitor&&) noexcept = default;

	const std::string& Monitor::property_name() const noexcept {
		return implementation_->property_name;
	}

	const std::string& Monitor::formula() const {
		if (implementation_->formula.empty())
			implementation_->formula = render_formula(implementation_->source_formula);
		return implementation_->formula;
	}

	Verdict Monitor::verdict() const noexcept {
		if (implementation_->unresolved) return Verdict::UNRESOLVED;
		return ltl::verdict(implementation_->residual);
	}

	std::optional<uint64_t> Monitor::repeats_until() const noexcept {
		const Implementation& state = *implementation_;
		if (!state.observed || state.finished || !state.stutter || verdict() != Verdict::PENDING) return std::nullopt;
		return state.wakeup;
	}

	uint64_t Monitor::activations_after_repeats(uint64_t count) const {
		const Implementation& state = *implementation_;
		if (count == 0) return state.activations;
		if (!repeats_until() || next_tick() > state.wakeup || count > state.wakeup - next_tick())
			throw std::logic_error("LTL monitor repeated past its certified self-loop");
		return state.activations + count * state.stutter_activations;
	}

	void Monitor::repeat(uint64_t count) {
		Implementation& state = *implementation_;
		state.activations = activations_after_repeats(count);
		state.observations += count;
	}

	uint64_t Monitor::next_tick() const noexcept {
		return implementation_->activation_tick + implementation_->observations;
	}

	bool Monitor::progressed() const noexcept {
		return implementation_->progressed;
	}

	uint64_t Monitor::activations() const noexcept {
		return implementation_->activations;
	}

	bool Monitor::has_trigger() const noexcept {
		return implementation_->trigger;
	}

	const std::string& Monitor::obligation_at_end() const noexcept {
		return implementation_->obligation_at_end;
	}

	const std::optional<FailureExplanation>& Monitor::failure() const noexcept {
		return implementation_->failure;
	}

	Verdict Monitor::observe(const AtomEvaluator& evaluate, const AtomValueRenderer& render_actual,
							 const DomainEvaluator& evaluate_domain, const StepEvaluator& evaluate_step,
							 uint64_t revision, const InputTracker* inputs) {
		if (implementation_->finished) throw std::logic_error("cannot observe a finished LTL monitor");
		implementation_->observed   = true;
		implementation_->progressed = false;
		const Verdict current       = verdict();
		if (current != Verdict::PENDING) return current;
		// A self-loop in progression can repeat only with identical evaluator inputs. This
		// expires at the next temporal boundary even if the input revision does not change.
		if (revision && revision == implementation_->last_revision && implementation_->stutter &&
			implementation_->activation_tick + implementation_->observations < implementation_->wakeup) {
			++implementation_->observations;
			implementation_->activations += implementation_->stutter_activations;
			return current;
		}
		implementation_->last_revision      = revision;
		implementation_->progressed         = true;
		const uint64_t previous_activations = implementation_->activations;
		const NodePtr  obligation           = implementation_->residual;
		const uint64_t observation_tick     = implementation_->activation_tick + implementation_->observations++;
		implementation_->transient_formulas.clear();
		implementation_->last_evaluations.clear();
		implementation_->wakeup = std::numeric_limits<uint64_t>::max();
		const FormulaOwner own  = [&](std::unique_ptr<Formula> formula, bool persistent) {
			if (persistent) implementation_->persistent_formulas.push_back(std::move(formula));
			else implementation_->transient_formulas.push_back(std::move(formula));
		};
		const PersistentGrounder ground = [&](const Formula& formula, const std::string& binding, uint64_t identity) {
			auto bindings = formula.ground_bindings;
			if (identity) bindings.emplace_back(binding, identity);
			auto    grounded   = clone_formula(identity ? *formula.left : *formula.right, bindings);
			NodePtr normalized = normalize(*grounded, implementation_->factory);
			implementation_->persistent_formulas.push_back(std::move(grounded));
			return normalized;
		};
		NodeFactory& factory = implementation_->factory;
		if (obligation->kind != NodeKind::AND || !inputs) {
			implementation_->residual = progress(obligation, evaluate, factory, evaluate_domain, own, ground,
												 evaluate_step, implementation_->activations, observation_tick,
												 implementation_->wakeup, implementation_->last_evaluations, inputs);
		} else {
			// The conjuncts of the top-level conjunction are independent obligations. Each one is
			// progressed on its own so that an unchanged obligation can repeat its cached outcome.
			// The conjunction is false as soon as one conjunct is, and otherwise only rebuilt when a
			// conjunct changed.
			std::vector<NodePtr> leaves;
			std::vector<NodePtr> stack{obligation};
			while (!stack.empty()) {
				NodePtr node = std::move(stack.back());
				stack.pop_back();
				if (node->kind == NodeKind::AND) {
					stack.push_back(node->right);
					stack.push_back(node->left);
				} else leaves.push_back(std::move(node));
			}
			std::vector<NodePtr> results;
			results.reserve(leaves.size());
			bool changed = false, refuted = false;
			for (const NodePtr& leaf : leaves) {
				NodePtr result = progress_cached(leaf, evaluate, factory, evaluate_domain, own, ground, evaluate_step,
												 implementation_->activations, observation_tick,
												 implementation_->wakeup, implementation_->last_evaluations, inputs);
				if (result->kind == NodeKind::FALSE) {
					refuted = true;
					break;
				}
				changed |= result != leaf;
				results.push_back(std::move(result));
			}
			if (refuted) implementation_->residual = factory.constant(false);
			else if (changed) {
				NodePtr residual = results.back();
				for (std::size_t index = results.size() - 1; index-- > 0;)
					residual = factory.make_and(results[index], std::move(residual));
				implementation_->residual = std::move(residual);
			}
		}
		if (implementation_->residual != obligation)
			implementation_->residual = subsume(implementation_->residual, implementation_->factory, observation_tick);
		implementation_->stutter             = implementation_->residual == obligation;
		implementation_->stutter_activations = implementation_->activations - previous_activations;
		if (verdict() == Verdict::FALSE) {
			std::vector<AtomFact> facts;
			facts.reserve(implementation_->last_evaluations.size());
			std::unordered_set<const Formula*> seen;
			for (const auto& [atom, value] : implementation_->last_evaluations) {
				if (seen.insert(atom).second)
					facts.push_back(
						{render_formula(*atom), value, render_actual ? render_actual(*atom) : std::nullopt});
			}
			FailureExplanation              explanation{formula(), render_node(obligation), std::move(facts), {}, {}};
			TimedObligation                 timed;
			std::unordered_set<const Node*> visited;
			find_timed_obligation(obligation, timed, visited, observation_tick);
			if (timed.formula && timed.elapsed <= observation_tick && timed.formula->interval) {
				explanation.obligation_created_tick = observation_tick - timed.elapsed;
				explanation.deadline_tick = *explanation.obligation_created_tick + timed.formula->interval->until_tick;
			}
			implementation_->failure = std::move(explanation);
		}
		if (implementation_->factory.size() >= implementation_->next_node_sweep ||
			implementation_->persistent_formulas.size() >= implementation_->next_formula_sweep) {
			implementation_->factory.prune_expired();
			implementation_->next_node_sweep = std::max(MAINTENANCE_NODES, 2 * implementation_->factory.size());
			if (implementation_->persistent_formulas.empty()) return verdict();
			// Ground formulas only need to outlive the residual nodes and diagnostics that refer to them.
			std::unordered_set<const Formula*> sources;
			std::unordered_set<const Node*>    visited;
			collect_formula_sources(implementation_->residual, sources, visited);
			collect_formula_sources(obligation, sources, visited);
			for (const auto& [formula, value] : implementation_->last_evaluations) {
				(void)value;
				sources.insert(formula);
			}
			std::erase_if(implementation_->persistent_formulas,
						  [&](const auto& formula) { return !formula_tree_is_referenced(*formula, sources); });
			implementation_->next_formula_sweep =
				std::max(MAINTENANCE_FORMULAS, 2 * implementation_->persistent_formulas.size());
		}
		return verdict();
	}

	Verdict Monitor::finish(const AtomValueRenderer& render_actual) {
		if (!implementation_->observed)
			throw std::logic_error("cannot finish an LTL monitor before its first observation");
		if (implementation_->finished) return verdict();
		implementation_->finished = true;
		if (verdict() == Verdict::PENDING) {
			const NodePtr obligation  = implementation_->residual;
			implementation_->residual = ltl::finish(implementation_->residual, implementation_->factory);

			// A refuted property whose obligation a longer trace could still have met was not shown
			// false by this trace; it simply ran out of trace. Report that as its own outcome rather
			// than as a failure the applet caused.
			if (verdict() == Verdict::FALSE &&
				ltl::verdict(ltl::finish(obligation, implementation_->factory, true)) == Verdict::TRUE) {
				implementation_->unresolved        = true;
				implementation_->obligation_at_end = render_node(obligation);
				implementation_->factory.prune_expired();
				return verdict();
			}
			if (verdict() == Verdict::FALSE) {
				std::vector<AtomFact> facts;
				facts.reserve(implementation_->last_evaluations.size());
				std::unordered_set<const Formula*> seen;
				for (const auto& [atom, value] : implementation_->last_evaluations) {
					if (seen.insert(atom).second)
						facts.push_back(
							{render_formula(*atom), value, render_actual ? render_actual(*atom) : std::nullopt});
				}
				FailureExplanation explanation{formula(), render_node(obligation), std::move(facts), {}, {}};
				TimedObligation    timed;
				std::unordered_set<const Node*> visited;
				const uint64_t final_tick = implementation_->activation_tick + implementation_->observations - 1;
				find_timed_obligation(obligation, timed, visited, final_tick);
				if (timed.formula && timed.formula->interval) {
					if (timed.elapsed <= final_tick) {
						explanation.obligation_created_tick = final_tick - timed.elapsed;
						explanation.deadline_tick =
							*explanation.obligation_created_tick + timed.formula->interval->until_tick;
					}
				}
				implementation_->failure = std::move(explanation);
			}
		}
		return verdict();
	}

} // namespace ltl
