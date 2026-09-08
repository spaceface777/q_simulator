#ifndef LTL_MONITOR_H
#define LTL_MONITOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ast.h"

namespace ltl {

	enum class Verdict {
		FALSE,
		TRUE,
		PENDING,

		// The trace ended with an obligation still open that a longer trace could have met. The
		// property was neither witnessed nor refuted, which is a different fact from failing: a
		// deadline that expired inside the trace is refuted, and stays FALSE.
		UNRESOLVED,
	};

	// Evaluates one typed ground atom or comparison against the current immutable observation.
	// The referenced source formula remains valid for the duration of the call.
	using AtomEvaluator     = std::function<bool(const Formula&)>;
	using AtomValueRenderer = std::function<std::optional<std::string>(const Formula&)>;
	using DomainVisitor     = std::function<void(uint64_t)>;
	// The Module domain is the fixed layout. Tub and RPC domains are observed on each tick.
	using DomainEvaluator = std::function<void(const Value*, ValueType, const DomainVisitor&)>;

	// Whether the given module's application code advanced on the current observation. Module zero
	// asks about the global observation tick, which always advances. An absent evaluator reports
	// every module as advancing, which is the correct reading when no clock model is in play.
	using StepEvaluator = std::function<bool(uint16_t)>;

	struct AtomFact {
		std::string                expression;
		bool                       value = false;
		std::optional<std::string> actual;
	};

	struct FailureExplanation {
		std::string             formula;
		std::string             obligation;
		std::vector<AtomFact>   facts;
		std::optional<uint64_t> obligation_created_tick;
		std::optional<uint64_t> deadline_tick;
	};

	class Monitor {
	public:
		// formula and its child nodes must remain unchanged and outlive this monitor.
		Monitor(std::string property_name, const Formula& formula, uint64_t activation_tick = 0);
		~Monitor();

		Monitor(Monitor&&) noexcept;
		Monitor& operator=(Monitor&&) noexcept;

		Monitor(const Monitor&)            = delete;
		Monitor& operator=(const Monitor&) = delete;

		[[nodiscard]] const std::string&                       property_name() const noexcept;
		[[nodiscard]] const std::string&                       formula() const noexcept;
		[[nodiscard]] Verdict                                  verdict() const noexcept;
		[[nodiscard]] const std::optional<FailureExplanation>& failure() const noexcept;

		// How many times the property actually imposed an obligation, so a pass that never tested
		// anything is visible rather than indistinguishable from a pass that did. Counts the
		// observations at which an implication's antecedent held, once per quantified record. Zero
		// on a property that holds no implication, where the count carries no meaning; ask
		// has_trigger() before reporting it.
		[[nodiscard]] uint64_t activations() const noexcept;
		[[nodiscard]] bool     has_trigger() const noexcept;

		// The obligation still open when an UNRESOLVED trace ended, rendered as source-like syntax.
		[[nodiscard]] const std::string& obligation_at_end() const noexcept;

		// Progresses the property by one observation. Calls after a decisive verdict are harmless.
		Verdict observe(const AtomEvaluator& evaluate, const AtomValueRenderer& render_actual = {},
						const DomainEvaluator& evaluate_domain = {}, const StepEvaluator& evaluate_step = {});

		// Applies finite-trace end semantics. A scenario trace always contains at least tick zero, so
		// finishing before the first observation is a programming error.
		Verdict finish(const AtomValueRenderer& render_actual = {});

	private:
		class Implementation;
		std::unique_ptr<Implementation> implementation_;
	};

} // namespace ltl

#endif // LTL_MONITOR_H
