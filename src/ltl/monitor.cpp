#include "monitor.h"

#include <cstddef>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ltl {

	namespace {

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
		};

		struct Node;
		using NodePtr = std::shared_ptr<const Node>;

		struct Node {
			NodeKind       kind;
			const Formula* source;
			NodePtr        left;
			NodePtr        right;
			uint64_t       elapsed = 0;
		};

		struct NodeKey {
			NodeKind       kind;
			const Formula* source;
			const Node*    left;
			const Node*    right;
			uint64_t       elapsed;

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
				result ^= std::hash<uint64_t>{}(key.elapsed) + 0x9e3779b9 + (result << 6) + (result >> 2);
				return result;
			}
		};

		class NodeFactory {
		private:
			std::unordered_map<NodeKey, std::weak_ptr<const Node>, NodeKeyHash> nodes_;
			std::unordered_map<std::string, std::weak_ptr<const Node>>          atoms_;

			NodePtr intern(NodeKind kind, const Formula* source = nullptr, NodePtr left = {}, NodePtr right = {},
						   uint64_t elapsed = 0) {
				const NodeKey key{kind, source, left.get(), right.get(), elapsed};
				if (const auto found = nodes_.find(key); found != nodes_.end()) {
					if (NodePtr existing = found->second.lock()) return existing;
				}
				auto node   = std::make_shared<Node>(Node{kind, source, std::move(left), std::move(right), elapsed});
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

			NodePtr atom(const Formula& source, const std::string& key) {
				if (const auto found = atoms_.find(key); found != atoms_.end()) {
					if (NodePtr existing = found->second.lock()) return existing;
				}
				NodePtr node = intern(NodeKind::ATOM, &source);
				atoms_[key]  = node;
				return node;
			}

			NodePtr unary(NodeKind kind, const Formula& source, NodePtr child) {
				return intern(kind, &source, std::move(child));
			}

			NodePtr binary(NodeKind kind, const Formula& source, NodePtr left, NodePtr right) {
				return intern(kind, &source, std::move(left), std::move(right));
			}

			NodePtr advance(const NodePtr& node) {
				return intern(node->kind, node->source, node->left, node->right, node->elapsed + 1);
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

		void append_value_key(std::ostringstream& output, const Value& value) {
			output << static_cast<unsigned>(value.kind) << ':' << static_cast<unsigned>(value.type) << ':';
			switch (value.kind) {
			case Value::Kind::BOOLEAN: output << value.boolean; break;
			case Value::Kind::NUMBER:  output << std::setprecision(17) << value.number; break;
			case Value::Kind::COLOR:   output << value.color; break;
			case Value::Kind::BYTES:
				output << value.bytes.size() << ':';
				for (const uint8_t byte : value.bytes) output << static_cast<unsigned>(byte) << ',';
				break;
			case Value::Kind::ENUM_LITERAL: output << '.' << value.text; break;
			case Value::Kind::COUNT:
				output << value.binding << ':' << static_cast<unsigned>(value.binding_type) << ':';
				if (value.domain) append_value_key(output, *value.domain);
				output << ':' << render_formula(*value.predicate);
				break;
			case Value::Kind::NAME: output << value.entity_id; break;
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
			}
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
			case Value::Kind::NUMBER:  output << std::setprecision(17) << value.number; break;
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
				return factory.make_and(normalize(*formula.left, factory), normalize(*formula.right, factory),
										&formula);
			case Formula::Kind::OR:
				return factory.make_or(normalize(*formula.left, factory), normalize(*formula.right, factory), &formula);
			case Formula::Kind::IMPLIES:
				return factory.make_or(factory.negate_antecedent(normalize(*formula.left, factory), &formula),
									   normalize(*formula.right, factory), &formula);
			case Formula::Kind::UNTIL:
				return factory.binary(NodeKind::UNTIL, formula, normalize(*formula.left, factory),
									  normalize(*formula.right, factory));
			case Formula::Kind::FORALL: return factory.unary(NodeKind::FORALL, formula, {});
			case Formula::Kind::EXISTS: return factory.unary(NodeKind::EXISTS, formula, {});
			}
			throw std::logic_error("unknown LTL formula kind");
		}

		std::unique_ptr<Value> clone_value(const Value& value, const std::string& binding, uint64_t identity);

		std::unique_ptr<Formula> clone_formula(const Formula& formula, const std::string& binding, uint64_t identity) {
			auto result          = std::make_unique<Formula>();
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
			if (!formula.step_module_binding.empty() && formula.step_module_binding == binding) {
				result->step_module = static_cast<uint16_t>(identity);
				result->step_module_binding.clear();
			}
			if (formula.value) result->value = clone_value(*formula.value, binding, identity);
			if (formula.compared) result->compared = clone_value(*formula.compared, binding, identity);
			if (formula.left) result->left = clone_formula(*formula.left, binding, identity);
			if (formula.right) result->right = clone_formula(*formula.right, binding, identity);
			if (formula.domain) result->domain = clone_value(*formula.domain, binding, identity);
			return result;
		}

		std::unique_ptr<Value> clone_value(const Value& value, const std::string& binding, uint64_t identity) {
			auto result          = std::make_unique<Value>();
			result->kind         = value.kind;
			result->span         = value.span;
			result->type         = value.type;
			result->text         = value.text;
			result->number       = value.number;
			result->color        = value.color;
			result->bytes        = value.bytes;
			result->entity_id    = value.entity_id;
			result->boolean      = value.boolean;
			result->binding      = value.binding;
			result->binding_type = value.binding_type;
			if (value.kind == Value::Kind::NAME && value.text == binding) {
				result->entity_id = identity;
			}
			if (value.base) result->base = clone_value(*value.base, binding, identity);
			for (const auto& argument : value.arguments) {
				result->arguments.push_back(clone_value(*argument, binding, identity));
			}
			if (value.domain) result->domain = clone_value(*value.domain, binding, identity);
			if (value.predicate) result->predicate = clone_formula(*value.predicate, binding, identity);
			return result;
		}

		using FormulaOwner       = std::function<void(std::unique_ptr<Formula>, bool)>;
		using PersistentGrounder = std::function<NodePtr(const Formula&, const std::string&, uint64_t)>;

		NodePtr progress(const NodePtr& node, const AtomEvaluator& evaluate, NodeFactory& factory,
						 const DomainEvaluator& evaluate_domain, const FormulaOwner& own_formula,
						 const PersistentGrounder& ground_persistent, const StepEvaluator& evaluate_step,
						 uint64_t& activations) {
			auto progress_child = [&](const NodePtr& child) {
				return progress(child, evaluate, factory, evaluate_domain, own_formula, ground_persistent,
								evaluate_step, activations);
			};
			switch (node->kind) {
			case NodeKind::FALSE: return factory.constant(false);
			case NodeKind::TRUE:  return factory.constant(true);
			case NodeKind::ATOM:  return factory.constant(evaluate(*node->source));
			case NodeKind::NOT:   {
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
				if (node->elapsed < node->source->interval->from_tick) return factory.advance(node);
				if (node->elapsed == node->source->interval->until_tick) {
					return progress_child(node->left);
				}
				{
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::FALSE) return current;
					return factory.make_and(std::move(current), factory.advance(node), node->source);
				}
			case NodeKind::EVENTUALLY:
				if (!node->source->interval) {
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::TRUE) return current;
					return factory.make_or(std::move(current), node, node->source);
				}
				if (node->elapsed < node->source->interval->from_tick) return factory.advance(node);
				if (node->elapsed == node->source->interval->until_tick) {
					return progress_child(node->left);
				}
				{
					NodePtr current = progress_child(node->left);
					if (current->kind == NodeKind::TRUE) return current;
					return factory.make_or(std::move(current), factory.advance(node), node->source);
				}
			case NodeKind::NEXT:
				// The observation the obligation was created on is the present, never the next step.
				if (node->elapsed == 0) return factory.advance(node);
				// A module that was not granted this tick has not advanced, so its next step is still
				// ahead. Hold the obligation rather than discharging it against a tick whose code did
				// not run. Module zero is the global tick, which advances on every observation.
				if (node->source->step_module != 0 && evaluate_step && !evaluate_step(node->source->step_module)) {
					return factory.advance(node);
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
				if (node->elapsed == node->source->interval->until_tick) {
					return progress_child(node->right);
				}
				if (node->elapsed < node->source->interval->from_tick) {
					NodePtr prefix = progress_child(node->left);
					if (prefix->kind == NodeKind::FALSE) return prefix;
					return factory.make_and(std::move(prefix), factory.advance(node), node->source);
				}
				{
					NodePtr witness = progress_child(node->right);
					if (witness->kind == NodeKind::TRUE) return witness;
					NodePtr prefix = progress_child(node->left);
					NodePtr continuation =
						prefix->kind == NodeKind::FALSE
							? prefix
							: factory.make_and(std::move(prefix), factory.advance(node), node->source);
					return factory.make_or(std::move(witness), std::move(continuation), node->source);
				}
			case NodeKind::FORALL:
			case NodeKind::EXISTS: {
				if (!evaluate_domain) throw std::logic_error("quantified monitor requires a domain evaluator");
				const bool universal = node->kind == NodeKind::FORALL;
				NodePtr    result    = factory.constant(universal);
				evaluate_domain(node->source->domain.get(), node->source->binding_type, [&](uint64_t identity) {
					if ((universal && result->kind == NodeKind::FALSE) ||
						(!universal && result->kind == NodeKind::TRUE))
						return;
					NodePtr item;
					if (!node->source->domain) {
						item = progress_child(ground_persistent(*node->source->left, node->source->binding, identity));
					} else {
						auto grounded         = clone_formula(*node->source->left, node->source->binding, identity);
						item                  = progress_child(normalize(*grounded, factory));
						const bool persistent = item->kind != NodeKind::TRUE && item->kind != NodeKind::FALSE;
						own_formula(std::move(grounded), persistent);
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
			case NodeKind::EXISTS:     throw std::logic_error("unprogressed quantifier at finite-trace end");
			}
			throw std::logic_error("unknown normalized LTL formula kind");
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
								   std::unordered_set<const Node*>& visited) {
			if (!visited.insert(node.get()).second) return;
			if (node->source && node->source->interval && (!result.formula || node->elapsed > result.elapsed)) {
				result = TimedObligation{node->source, node->elapsed};
			}
			if (node->left) find_timed_obligation(node->left, result, visited);
			if (node->right) find_timed_obligation(node->right, result, visited);
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
			: property_name(std::move(property_name)), formula(render_formula(formula)),
			  residual(normalize(formula, factory)), trigger(contains_implication(formula)),
			  activation_tick(activation_tick) {}

		std::string                                            property_name;
		std::string                                            formula;
		NodeFactory                                            factory;
		NodePtr                                                residual;
		std::vector<std::pair<const Formula*, bool>>           last_evaluations;
		std::vector<std::unique_ptr<Formula>>                  transient_formulas;
		std::vector<std::unique_ptr<Formula>>                  persistent_formulas;
		std::vector<std::unique_ptr<Formula>>                  persistent_ground_formulas;
		std::map<std::pair<const Formula*, uint64_t>, NodePtr> persistent_ground_nodes;
		std::optional<FailureExplanation>                      failure;
		std::string                                            obligation_at_end;
		bool                                                   observed        = false;
		bool                                                   finished        = false;
		bool                                                   unresolved      = false;
		bool                                                   trigger         = false;
		uint64_t                                               activation_tick = 0;
		uint64_t                                               observations    = 0;
		uint64_t                                               activations     = 0;
	};

	Monitor::Monitor(std::string property_name, const Formula& formula, uint64_t activation_tick)
		: implementation_(std::make_unique<Implementation>(std::move(property_name), formula, activation_tick)) {}

	Monitor::~Monitor() = default;

	Monitor::Monitor(Monitor&&) noexcept = default;

	Monitor& Monitor::operator=(Monitor&&) noexcept = default;

	const std::string& Monitor::property_name() const noexcept {
		return implementation_->property_name;
	}

	const std::string& Monitor::formula() const noexcept {
		return implementation_->formula;
	}

	Verdict Monitor::verdict() const noexcept {
		if (implementation_->unresolved) return Verdict::UNRESOLVED;
		return ltl::verdict(implementation_->residual);
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
							 const DomainEvaluator& evaluate_domain, const StepEvaluator& evaluate_step) {
		if (implementation_->finished) throw std::logic_error("cannot observe a finished LTL monitor");
		implementation_->observed = true;
		const Verdict current     = verdict();
		if (current != Verdict::PENDING) return current;
		const NodePtr  obligation       = implementation_->residual;
		const uint64_t observation_tick = implementation_->activation_tick + implementation_->observations++;
		implementation_->transient_formulas.clear();
		implementation_->last_evaluations.clear();
		std::unordered_set<const Formula*> seen;
		implementation_->residual = progress(
			implementation_->residual,
			[&](const Formula& atom) {
				const bool value = evaluate(atom);
				if (seen.insert(&atom).second) implementation_->last_evaluations.emplace_back(&atom, value);
				return value;
			},
			implementation_->factory, evaluate_domain,
			[&](std::unique_ptr<Formula> formula, bool persistent) {
				if (persistent) implementation_->persistent_formulas.push_back(std::move(formula));
				else implementation_->transient_formulas.push_back(std::move(formula));
			},
			[&](const Formula& formula, const std::string& binding, uint64_t identity) {
				const auto key = std::make_pair(&formula, identity);
				if (const auto found = implementation_->persistent_ground_nodes.find(key);
					found != implementation_->persistent_ground_nodes.end()) {
					return found->second;
				}
				auto    grounded   = clone_formula(formula, binding, identity);
				NodePtr normalized = normalize(*grounded, implementation_->factory);
				implementation_->persistent_ground_formulas.push_back(std::move(grounded));
				implementation_->persistent_ground_nodes.emplace(key, normalized);
				return normalized;
			},
			evaluate_step, implementation_->activations);
		if (verdict() == Verdict::FALSE) {
			std::vector<AtomFact> facts;
			facts.reserve(implementation_->last_evaluations.size());
			for (const auto& [atom, value] : implementation_->last_evaluations) {
				facts.push_back({render_formula(*atom), value, render_actual ? render_actual(*atom) : std::nullopt});
			}
			FailureExplanation explanation{implementation_->formula, render_node(obligation), std::move(facts), {}, {}};
			TimedObligation    timed;
			std::unordered_set<const Node*> visited;
			find_timed_obligation(obligation, timed, visited);
			if (timed.formula && timed.elapsed <= observation_tick && timed.formula->interval) {
				explanation.obligation_created_tick = observation_tick - timed.elapsed;
				explanation.deadline_tick = *explanation.obligation_created_tick + timed.formula->interval->until_tick;
			}
			implementation_->failure = std::move(explanation);
		}
		implementation_->factory.prune_expired();
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
				for (const auto& [atom, value] : implementation_->last_evaluations) {
					facts.push_back(
						{render_formula(*atom), value, render_actual ? render_actual(*atom) : std::nullopt});
				}
				FailureExplanation explanation{
					implementation_->formula, render_node(obligation), std::move(facts), {}, {}};
				TimedObligation                 timed;
				std::unordered_set<const Node*> visited;
				find_timed_obligation(obligation, timed, visited);
				if (timed.formula && timed.formula->interval) {
					const uint64_t final_tick = implementation_->activation_tick + implementation_->observations - 1;
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
