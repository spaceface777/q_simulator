#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../src/ltl/checker.h"
#include "../../src/ltl/monitor.h"
#include "../../src/ltl/parser.h"

namespace {

	struct CompiledProperty {
		ltl::File file;

		explicit CompiledProperty(const std::string& formula) {
			file = ltl::parse("prop test = " + formula + ";", "monitor.spec");
			ltl::type_check(file, ltl::TypeEnvironment{{1}}, "monitor.spec");
		}

		const ltl::Formula& formula() const { return *file.properties[0].formula; }
	};

	using Observation = std::unordered_map<std::string, bool>;

	bool evaluate(const ltl::Formula& atom, const Observation& observation) {
		if (atom.kind == ltl::Formula::Kind::ATOM && atom.value->kind == ltl::Value::Kind::BOOLEAN) {
			return atom.value->boolean;
		}
		if (atom.kind != ltl::Formula::Kind::ATOM || atom.value->kind != ltl::Value::Kind::MEMBER) {
			throw std::logic_error("test evaluator received an unexpected atom");
		}
		return observation.at(atom.value->text);
	}

	// Drives a monitor the way the scenario runtime does: it records the atoms each step reads, keeps
	// its input revision while none of them changed, and lets the monitor reuse unchanged obligations.
	class TrackedMonitor {
	public:
		explicit TrackedMonitor(const ltl::Formula& formula) : monitor_("tracked", formula) {}

		ltl::Verdict observe(const Observation& observation, bool granted = true) {
			const uint64_t tick = ticks_++;
			if (previous_) {
				for (const auto& [name, value] : observation)
					if (previous_->at(name) != value) changed_[channel(name)] = tick;
				if (granted != previous_granted_) changed_[STEP_CHANNEL] = tick;
			}
			previous_         = observation;
			previous_granted_ = granted;
			if (!read_tick_ || changed_since(reads_, *read_tick_)) ++revision_;
			std::vector<uint32_t>   scratch;
			const ltl::InputTracker inputs{&scratch,
										   [this](const std::vector<uint32_t>& channels, uint64_t since) {
											   return changed_since(channels, since);
										   },
										   tick,
										   {},
										   {}};
			const auto              atom = [&](const ltl::Formula& formula) {
				if (formula.value->kind == ltl::Value::Kind::MEMBER) scratch.push_back(channel(formula.value->text));
				return evaluate(formula, observation);
			};
			const auto step = [&](uint16_t) {
				scratch.push_back(STEP_CHANNEL);
				return granted;
			};
			const ltl::Verdict verdict = monitor_.observe(atom, {}, {}, step, revision_, &inputs);
			if (monitor_.progressed()) {
				reads_     = std::move(scratch);
				read_tick_ = tick;
			}
			return verdict;
		}

		ltl::Monitor& monitor() { return monitor_; }

	private:
		static constexpr uint32_t STEP_CHANNEL = 0;

		uint32_t channel(const std::string& name) {
			return channels_.try_emplace(name, static_cast<uint32_t>(channels_.size() + 1)).first->second;
		}

		bool changed_since(const std::vector<uint32_t>& channels, uint64_t since) const {
			for (const uint32_t read : channels) {
				const auto found = changed_.find(read);
				if (found != changed_.end() && found->second > since) return true;
			}
			return false;
		}

		ltl::Monitor                              monitor_;
		std::unordered_map<std::string, uint32_t> channels_;
		std::unordered_map<uint32_t, uint64_t>    changed_;
		std::optional<Observation>                previous_;
		bool                                      previous_granted_ = true;
		std::vector<uint32_t>                     reads_;
		std::optional<uint64_t>                   read_tick_;
		uint64_t                                  revision_ = 0;
		uint64_t                                  ticks_    = 0;
	};

	std::vector<ltl::Verdict> run(const std::string& formula, const std::vector<Observation>& observations) {
		CompiledProperty          property(formula);
		ltl::Monitor              monitor("test", property.formula());
		ltl::Monitor              cached("cached", property.formula());
		TrackedMonitor            tracked(property.formula());
		std::vector<ltl::Verdict> verdicts;
		uint64_t                  revision = 1;
		const Observation*        previous = nullptr;
		for (const Observation& observation : observations) {
			if (previous && *previous != observation) ++revision;
			previous        = &observation;
			const auto atom = [&observation](const ltl::Formula& formula) { return evaluate(formula, observation); };
			verdicts.push_back(monitor.observe(atom));
			EXPECT_EQ(cached.observe(atom, {}, {}, {}, revision), verdicts.back());
			EXPECT_EQ(cached.activations(), monitor.activations());
			EXPECT_EQ(tracked.observe(observation), verdicts.back());
			EXPECT_EQ(tracked.monitor().activations(), monitor.activations());
		}
		verdicts.push_back(monitor.finish());
		EXPECT_EQ(cached.finish(), verdicts.back());
		EXPECT_EQ(tracked.monitor().finish(), verdicts.back());
		return verdicts;
	}

	// Drives the monitor with a per-observation record of whether the module's own clock granted the
	// tick, so `next` can be tested against a module that misses ticks.
	std::vector<ltl::Verdict> run_with_steps(const std::string& formula, const std::vector<Observation>& observations,
											 const std::vector<bool>& stepped) {
		CompiledProperty          property(formula);
		ltl::Monitor              monitor("test", property.formula());
		ltl::Monitor              cached("cached steps", property.formula());
		TrackedMonitor            tracked(property.formula());
		std::vector<ltl::Verdict> verdicts;
		uint64_t                  revision = 1;
		for (std::size_t tick = 0; tick < observations.size(); tick++) {
			const Observation& observation = observations[tick];
			const bool         granted     = stepped[tick];
			if (tick && (observations[tick - 1] != observation || stepped[tick - 1] != granted)) ++revision;
			const auto atom = [&observation](const ltl::Formula& formula) { return evaluate(formula, observation); };
			const auto step = [granted](uint16_t) { return granted; };
			verdicts.push_back(monitor.observe(atom, {}, {}, step));
			EXPECT_EQ(cached.observe(atom, {}, {}, step, revision), verdicts.back());
			EXPECT_EQ(cached.activations(), monitor.activations());
			EXPECT_EQ(tracked.observe(observation, granted), verdicts.back());
			EXPECT_EQ(tracked.monitor().activations(), monitor.activations());
		}
		verdicts.push_back(monitor.finish());
		EXPECT_EQ(cached.finish(), verdicts.back());
		EXPECT_EQ(tracked.monitor().finish(), verdicts.back());
		return verdicts;
	}

	Observation signals(bool p, bool q = false) {
		return {{"rfid_active", p}, {"left_laser_enabled", q}};
	}

	// UNRESOLVED refines FALSE rather than replacing it: it marks the subset of refutations that only
	// the end of the trace produced. Collapsing it recovers the two-valued LTLf reading.
	ltl::Verdict two_valued(ltl::Verdict verdict) {
		return verdict == ltl::Verdict::UNRESOLVED ? ltl::Verdict::FALSE : verdict;
	}

	bool reference(const ltl::Formula& formula, const std::vector<Observation>& trace, std::size_t position) {
		switch (formula.kind) {
		case ltl::Formula::Kind::ATOM:       return evaluate(formula, trace[position]);
		case ltl::Formula::Kind::COMPARISON: return evaluate(formula, trace[position]);
		case ltl::Formula::Kind::NOT:        return !reference(*formula.left, trace, position);
		case ltl::Formula::Kind::AND:
			return reference(*formula.left, trace, position) && reference(*formula.right, trace, position);
		case ltl::Formula::Kind::OR:
			return reference(*formula.left, trace, position) || reference(*formula.right, trace, position);
		case ltl::Formula::Kind::IMPLIES:
			return !reference(*formula.left, trace, position) || reference(*formula.right, trace, position);
		case ltl::Formula::Kind::ALWAYS:
			for (std::size_t tick = position + (formula.interval ? formula.interval->from_tick : 0);
				 tick < trace.size() && (!formula.interval || tick <= position + formula.interval->until_tick);
				 tick++) {
				if (!reference(*formula.left, trace, tick)) return false;
			}
			return true;
		case ltl::Formula::Kind::EVENTUALLY:
			for (std::size_t tick = position + (formula.interval ? formula.interval->from_tick : 0);
				 tick < trace.size() && (!formula.interval || tick <= position + formula.interval->until_tick);
				 tick++) {
				if (reference(*formula.left, trace, tick)) return true;
			}
			return false;
		case ltl::Formula::Kind::NEXT:
			return position + 1 < trace.size() && reference(*formula.left, trace, position + 1);
		case ltl::Formula::Kind::UNTIL:
			for (std::size_t witness = position + (formula.interval ? formula.interval->from_tick : 0);
				 witness < trace.size() && (!formula.interval || witness <= position + formula.interval->until_tick);
				 witness++) {
				if (!reference(*formula.right, trace, witness)) continue;
				bool prefix = true;
				for (std::size_t tick = position; tick < witness; tick++) {
					prefix = prefix && reference(*formula.left, trace, tick);
				}
				if (prefix) return true;
			}
			return false;
		case ltl::Formula::Kind::FORALL:
		case ltl::Formula::Kind::EXISTS: throw std::logic_error("reference helper requires ground formulas");
		}
		throw std::logic_error("unknown test formula");
	}

} // namespace

TEST(LtlMonitor, ReusesStableInputsWithoutLosingActivationsOrDeadlineBoundaries) {
	CompiledProperty property("G[0ms, 2049ms] (true -> module(1).rfid_active)");
	ltl::Monitor     monitor("reuse", property.formula(), 100);
	unsigned         evaluations = 0;
	for (unsigned tick = 0; tick <= 2049; ++tick) {
		const auto result = monitor.observe(
			[&](const ltl::Formula&) {
				++evaluations;
				return true;
			},
			{}, {}, {}, 1);
		EXPECT_EQ(result, tick < 2049 ? ltl::Verdict::PENDING : ltl::Verdict::TRUE);
	}
	EXPECT_EQ(monitor.activations(), 2050u);
	EXPECT_LT(evaluations, 5u);
}

TEST(LtlMonitor, BulkRepeatsPreserveTriggersAndAnInclusiveDeadline) {
	CompiledProperty property("G[0ms, 20ms] (true -> module(1).rfid_active)");
	ltl::Monitor     bulk("bulk", property.formula(), 100);
	ltl::Monitor     reference("reference", property.formula(), 100);
	unsigned         evaluations = 0;
	const auto       atom        = [&](const ltl::Formula&) {
		++evaluations;
		return true;
	};
	EXPECT_FALSE(bulk.repeats_until());
	EXPECT_THROW(bulk.repeat(1), std::logic_error);
	EXPECT_EQ(bulk.observe(atom, {}, {}, {}, 1), ltl::Verdict::PENDING);
	EXPECT_EQ(bulk.observe(atom, {}, {}, {}, 1), ltl::Verdict::PENDING);
	ASSERT_EQ(bulk.repeats_until(), 120u);
	EXPECT_EQ(bulk.next_tick(), 102u);
	EXPECT_EQ(bulk.activations_after_repeats(18), 20u);
	EXPECT_EQ(bulk.activations(), 2u) << "Reporting repetitions must not mutate the monitor";
	EXPECT_THROW(bulk.repeat(19), std::logic_error);
	EXPECT_EQ(bulk.next_tick(), 102u);
	bulk.repeat(18);
	EXPECT_EQ(bulk.next_tick(), 120u);
	EXPECT_EQ(bulk.observe(atom, {}, {}, {}, 1), ltl::Verdict::TRUE);
	EXPECT_FALSE(bulk.repeats_until());
	EXPECT_THROW(bulk.repeat(1), std::logic_error);
	EXPECT_LE(evaluations, 3u);
	for (uint64_t tick = 100; tick <= 120; tick++) (void)reference.observe([](const ltl::Formula&) { return true; });
	EXPECT_EQ(bulk.verdict(), reference.verdict());
	EXPECT_EQ(bulk.activations(), reference.activations());
	EXPECT_EQ(bulk.activations(), 21u);
}

TEST(LtlMonitor, BulkRepeatsWakeAtBothMetricWindowBoundaries) {
	CompiledProperty property("F[4ms, 6ms] module(1).rfid_active");
	ltl::Monitor     monitor("window", property.formula(), 30);
	unsigned         evaluations = 0;
	const auto       atom        = [&](const ltl::Formula&) {
		++evaluations;
		return false;
	};
	(void)monitor.observe(atom, {}, {}, {}, 1);
	(void)monitor.observe(atom, {}, {}, {}, 1);
	ASSERT_EQ(monitor.repeats_until(), 34u);
	EXPECT_THROW(monitor.repeat(3), std::logic_error);
	monitor.repeat(2);
	EXPECT_EQ(monitor.next_tick(), 34u);
	EXPECT_EQ(evaluations, 0u);
	EXPECT_EQ(monitor.observe(atom, {}, {}, {}, 1), ltl::Verdict::PENDING);
	ASSERT_EQ(monitor.repeats_until(), 36u);
	monitor.repeat(1);
	EXPECT_EQ(monitor.next_tick(), 36u);
	EXPECT_EQ(monitor.observe(atom, {}, {}, {}, 1), ltl::Verdict::FALSE);
	EXPECT_EQ(evaluations, 2u);
	ASSERT_TRUE(monitor.failure());
	EXPECT_EQ(monitor.failure()->deadline_tick, 36u);
}

TEST(LtlMonitor, StableInputsCannotDelayADeadlineAtANonzeroActivationTick) {
	CompiledProperty property("F[2049ms, 2049ms] module(1).rfid_active");
	ltl::Monitor     monitor("deadline", property.formula(), 100);
	unsigned         evaluations = 0;
	for (unsigned tick = 0; tick <= 2049; ++tick) {
		EXPECT_EQ(monitor.observe(
					  [&](const ltl::Formula&) {
						  ++evaluations;
						  return false;
					  },
					  {}, {}, {}, 1),
				  tick < 2049 ? ltl::Verdict::PENDING : ltl::Verdict::FALSE);
	}
	EXPECT_EQ(evaluations, 1u);
	ASSERT_TRUE(monitor.failure());
	EXPECT_EQ(monitor.failure()->obligation_created_tick, 100u);
	EXPECT_EQ(monitor.failure()->deadline_tick, 2149u);
	ASSERT_EQ(monitor.failure()->facts.size(), 1u);
	EXPECT_FALSE(monitor.failure()->facts[0].value);
}

TEST(LtlMonitor, InputChangesWakeAMonitorBeforeItsNextTemporalBoundary) {
	CompiledProperty property("G[20ms, 40ms] module(1).rfid_active");
	ltl::Monitor     monitor("input wakeup", property.formula());
	for (unsigned tick = 0; tick <= 25; ++tick) {
		EXPECT_EQ(monitor.observe([&](const ltl::Formula&) { return tick < 25; }, {}, {}, {}, tick < 25 ? 1 : 2),
				  tick < 25 ? ltl::Verdict::PENDING : ltl::Verdict::FALSE);
	}
}

TEST(LtlMonitor, SpecializesStaticModuleGuardsOnceWithoutEvaluatingDeadBodies) {
	CompiledProperty property("G (forall m: Module: m != module(1) -> (F[1ms, 2ms] m.rfid_active))");
	ltl::Monitor     monitor("static guard", property.formula());
	unsigned         domains = 0;
	for (unsigned tick = 0; tick < 2050; ++tick) {
		EXPECT_EQ(monitor.observe(
					  [](const ltl::Formula&) -> bool {
						  ADD_FAILURE() << "A statically false module guard must erase its body";
						  return false;
					  },
					  {},
					  [&](const ltl::Value*, ltl::ValueType, const ltl::DomainVisitor& visit) {
						  ++domains;
						  visit(1);
					  }),
				  ltl::Verdict::PENDING);
	}
	EXPECT_EQ(domains, 1u);
	EXPECT_EQ(monitor.activations(), 0u);
	EXPECT_EQ(monitor.finish(), ltl::Verdict::TRUE);
}

TEST(LtlMonitor, SpecializationPreservesTriggersAndDeadlinesOnUnchangedTicks) {
	CompiledProperty property("G (forall m: Module: m == module(1) -> (F[1ms, 2ms] m.rfid_active))");
	ltl::Monitor     monitor("deadline", property.formula());
	for (unsigned tick = 0; tick < 3; ++tick) {
		EXPECT_EQ(monitor.observe([](const ltl::Formula&) { return false; }, {},
								  [](const ltl::Value*, ltl::ValueType, const ltl::DomainVisitor& visit) { visit(1); }),
				  tick < 2 ? ltl::Verdict::PENDING : ltl::Verdict::FALSE);
	}
	EXPECT_GT(monitor.activations(), 0u);
	ASSERT_TRUE(monitor.failure());
	EXPECT_EQ(monitor.failure()->deadline_tick, 2u);
}

TEST(LtlMonitor, EvaluatesBooleanTruthTables) {
	EXPECT_EQ(run("true && true", {signals(false)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run("true && false", {signals(false)}).back(), ltl::Verdict::FALSE);
	EXPECT_EQ(run("false || true", {signals(false)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run("false || false", {signals(false)}).back(), ltl::Verdict::FALSE);
	EXPECT_EQ(run("!true", {signals(false)}).back(), ltl::Verdict::FALSE);
	EXPECT_EQ(run("!false", {signals(false)}).back(), ltl::Verdict::TRUE);
}

TEST(LtlMonitor, NormalizesImplication) {
	EXPECT_EQ(run("true -> false", {signals(false)}).back(), ltl::Verdict::FALSE);
	EXPECT_EQ(run("false -> false", {signals(false)}).back(), ltl::Verdict::TRUE);
}

TEST(LtlMonitor, FoldsConstantsAndDuplicateAtoms) {
	CompiledProperty property("module(1).rfid_active && module(1).rfid_active && true");
	ltl::Monitor     monitor("test", property.formula());
	int              evaluations = 0;

	EXPECT_EQ(monitor.observe([&evaluations](const ltl::Formula&) {
		evaluations++;
		return true;
	}),
			  ltl::Verdict::TRUE);
	EXPECT_EQ(evaluations, 1);
}

TEST(LtlMonitor, FoldsDecisiveConstantsWithoutEvaluatingTheOtherOperand) {
	for (const auto& [formula, expected] : {
			 std::pair{"false -> (always[0ms, 10s] module(1).rfid_active)", ltl::Verdict::TRUE},
			 std::pair{"true || (always[0ms, 10s] module(1).rfid_active)", ltl::Verdict::TRUE},
			 std::pair{"(always[0ms, 10s] module(1).rfid_active) || true", ltl::Verdict::TRUE},
			 std::pair{"false && (always[0ms, 10s] module(1).rfid_active)", ltl::Verdict::FALSE},
			 std::pair{"(always[0ms, 10s] module(1).rfid_active) && false", ltl::Verdict::FALSE},
		 }) {
		CompiledProperty property(formula);
		ltl::Monitor     monitor("test", property.formula());

		EXPECT_EQ(monitor.observe([](const ltl::Formula&) -> bool {
			throw std::logic_error("constant-folded operand was evaluated");
		}),
				  expected);
	}
}

TEST(LtlMonitor, ShortCircuitsBooleanOperandsBeforeProgressingTemporalBranches) {
	for (const std::string formula : {
			 "always (module(1).rfid_active -> (always[0ms, 10s] module(1).left_laser_enabled))",
			 "always (module(1).rfid_active || (always[0ms, 10s] module(1).left_laser_enabled))",
		 }) {
		CompiledProperty property(formula);
		ltl::Monitor     monitor("test", property.formula());

		for (int tick = 0; tick < 10000; tick++) {
			int evaluations = 0;
			EXPECT_EQ(monitor.observe([&evaluations, &formula](const ltl::Formula& atom) {
				evaluations++;
				if (atom.value->text == "rfid_active") return formula.find("->") == std::string::npos;
				throw std::logic_error("short-circuited temporal branch was evaluated");
			}),
					  ltl::Verdict::PENDING);
			EXPECT_EQ(evaluations, 1);
		}
	}
}

TEST(LtlMonitor, ShortCircuitsFalseConjunctionsAndSatisfiedUntilWitnesses) {
	for (const std::string formula : {
			 "module(1).rfid_active && (always[0ms, 10s] module(1).left_laser_enabled)",
			 "(always[0ms, 10s] module(1).left_laser_enabled) until module(1).rfid_active",
		 }) {
		CompiledProperty property(formula);
		ltl::Monitor     monitor("test", property.formula());
		int              evaluations = 0;

		const ltl::Verdict verdict = monitor.observe([&evaluations, &formula](const ltl::Formula& atom) {
			evaluations++;
			if (atom.value->text == "rfid_active") return formula.find("until") != std::string::npos;
			throw std::logic_error("short-circuited temporal branch was evaluated");
		});

		EXPECT_EQ(verdict, formula.find("until") == std::string::npos ? ltl::Verdict::FALSE : ltl::Verdict::TRUE);
		EXPECT_EQ(evaluations, 1);
	}
}

TEST(LtlMonitor, ShortCircuitsQuantifiersBehindDecisiveBooleanOperands) {
	CompiledProperty property("false -> (forall module: Module: module.rfid_active)");
	ltl::Monitor     monitor("short circuit quantifier", property.formula());

	EXPECT_EQ(
		monitor.observe([](const ltl::Formula&) -> bool { throw std::logic_error("atom should not be evaluated"); }, {},
						[](const ltl::Value*, ltl::ValueType, const ltl::DomainVisitor&) {
							throw std::logic_error("domain should not be evaluated");
						}),
		ltl::Verdict::TRUE);
}

TEST(LtlMonitor, AlwaysFailsEarlyAndSucceedsOnlyAtEnd) {
	const std::string formula = "always module(1).rfid_active";

	EXPECT_EQ(run(formula, {signals(true), signals(true)}),
			  (std::vector{ltl::Verdict::PENDING, ltl::Verdict::PENDING, ltl::Verdict::TRUE}));
	EXPECT_EQ(run(formula, {signals(true), signals(false), signals(true)}),
			  (std::vector{ltl::Verdict::PENDING, ltl::Verdict::FALSE, ltl::Verdict::FALSE, ltl::Verdict::FALSE}));
}

TEST(LtlMonitor, EventuallySucceedsEarlyAndIsUndecidedOnlyAtEnd) {
	const std::string formula = "eventually module(1).rfid_active";

	EXPECT_EQ(run(formula, {signals(false), signals(true), signals(false)}),
			  (std::vector{ltl::Verdict::PENDING, ltl::Verdict::TRUE, ltl::Verdict::TRUE, ltl::Verdict::TRUE}));
	EXPECT_EQ(run(formula, {signals(false), signals(false)}),
			  (std::vector{ltl::Verdict::PENDING, ltl::Verdict::PENDING, ltl::Verdict::UNRESOLVED}));
}

TEST(LtlMonitor, StrongNextEvaluatesExactlyTheFollowingObservation) {
	EXPECT_EQ(run("next module(1).rfid_active", {signals(false), signals(true)}),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::TRUE, ltl::Verdict::TRUE}));
	EXPECT_EQ(run("next module(1).rfid_active", {signals(true), signals(false)}),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::FALSE, ltl::Verdict::FALSE}));
}

TEST(LtlMonitor, StrongNextIsUndecidedAtTheFiniteEndAndDerivesWeakNext) {
	EXPECT_EQ(run("next true", {signals(false)}),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::UNRESOLVED}));
	EXPECT_EQ(run("!next !module(1).rfid_active", {signals(false)}),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::TRUE}));
}

TEST(LtlMonitor, NestedStrongNextCountsLogicalObservations) {
	EXPECT_EQ(run("next (next module(1).rfid_active)", {signals(true), signals(false), signals(true)}),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::PENDING, ltl::Verdict::TRUE,
										 ltl::Verdict::TRUE}));
}

TEST(LtlMonitor, StrongNextSkipsObservationsWhereItsModuleDidNotStep) {
	// The module runs only on the third observation, so `next` must reach past the two ticks its
	// clock skipped rather than discharging against code that never executed.
	const std::vector<Observation> trace   = {signals(false), signals(false), signals(true)};
	const std::vector<bool>        stepped = {true, false, true};

	EXPECT_EQ(run_with_steps("next module(1).rfid_active", trace, stepped),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::PENDING, ltl::Verdict::TRUE,
										 ltl::Verdict::TRUE}));

	// Under the global tick the same trace resolves against the skipped second observation instead.
	EXPECT_EQ(run("next module(1).rfid_active", trace),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::FALSE, ltl::Verdict::FALSE,
										 ltl::Verdict::FALSE}));
}

TEST(LtlMonitor, StrongNextWithoutAModuleFollowsTheGlobalObservationTick) {
	// A formula naming no module speaks about the physical world, which advances on every tick even
	// while an application's clock is skipping.
	const std::vector<bool> stepped = {false, false};
	EXPECT_EQ(run_with_steps("next true", {signals(false), signals(false)}, stepped),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::TRUE, ltl::Verdict::TRUE}));
}

TEST(LtlMonitor, MetricEventuallyKeepsAdvancingWhenItsModuleDoesNotStep) {
	// Metric bounds describe shared physical time even when the atom names a module. The module's
	// observable RFID input can change while its code and local clock are stopped.
	const std::vector<Observation> trace   = {signals(false), signals(true)};
	const std::vector<bool>        stepped = {true, false};
	EXPECT_EQ(run_with_steps("eventually[1ms, 1ms] module(1).rfid_active", trace, stepped),
			  (std::vector<ltl::Verdict>{ltl::Verdict::PENDING, ltl::Verdict::TRUE, ltl::Verdict::TRUE}));
}

TEST(LtlMonitor, StrongNextIsUnchangedWhenEveryObservationIsAStep) {
	for (std::size_t length = 1; length <= 4; length++) {
		const std::size_t combinations = std::size_t{1} << length;
		for (std::size_t bits = 0; bits < combinations; bits++) {
			std::vector<Observation> trace;
			for (std::size_t tick = 0; tick < length; tick++) trace.push_back(signals((bits & (1u << tick)) != 0));
			EXPECT_EQ(run_with_steps("next module(1).rfid_active", trace, std::vector<bool>(length, true)),
					  run("next module(1).rfid_active", trace));
		}
	}
}

TEST(LtlMonitor, StrongNextMatchesTheExactOneTickMetricEventually) {
	for (std::size_t length = 1; length <= 4; length++) {
		const std::size_t combinations = std::size_t{1} << length;
		for (std::size_t bits = 0; bits < combinations; bits++) {
			std::vector<Observation> trace;
			for (std::size_t tick = 0; tick < length; tick++) trace.push_back(signals((bits & (1u << tick)) != 0));
			EXPECT_EQ(run("next module(1).rfid_active", trace),
					  run("eventually[1ms, 1ms] module(1).rfid_active", trace));
		}
	}
}

TEST(LtlMonitor, SeparatesAnUndecidedEventualityFromARefutedOne) {
	// An open eventuality could have been met by a longer trace, so the trace ended too early to
	// decide rather than showing the property false.
	EXPECT_EQ(run("eventually module(1).rfid_active", {signals(false), signals(false)}).back(),
			  ltl::Verdict::UNRESOLVED);

	// A deadline that expired inside the trace is decided by the trace itself, and stays refuted.
	EXPECT_EQ(
		run("eventually[0ms, 1ms] module(1).rfid_active", {signals(false), signals(false), signals(false)}).back(),
		ltl::Verdict::FALSE);

	// A safety property the trace never violated still passes, and a witnessed eventuality still
	// passes: only the genuinely undecided case is reclassified.
	EXPECT_EQ(run("always module(1).rfid_active", {signals(true), signals(true)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run("eventually module(1).rfid_active", {signals(false), signals(true)}).back(), ltl::Verdict::TRUE);
}

TEST(LtlMonitor, LeavesNegatedEventualitiesAndWeakNextDecided) {
	// `!eventually p` is a safety claim: never witnessing p is a pass, not an undecided trace.
	EXPECT_EQ(run("!eventually module(1).rfid_active", {signals(false), signals(false)}).back(), ltl::Verdict::TRUE);
	// The weak-next derivation is unchanged.
	EXPECT_EQ(run("!next !module(1).rfid_active", {signals(false)}).back(), ltl::Verdict::TRUE);
	// A bare strong next off the end of the trace is undecided, not refuted.
	EXPECT_EQ(run("next true", {signals(false)}).back(), ltl::Verdict::UNRESOLVED);
}

TEST(LtlMonitor, CountsTheObservationsAtWhichAConditionalPropertyFired) {
	CompiledProperty property("always (module(1).left_laser_enabled -> module(1).rfid_active)");
	ltl::Monitor     monitor("test", property.formula());
	EXPECT_TRUE(monitor.has_trigger());

	// Two of the four observations have a true antecedent, so the property was tested twice.
	for (const Observation& observation :
		 {signals(true, true), signals(true, false), signals(true, true), signals(false, false)}) {
		(void)monitor.observe([&observation](const ltl::Formula& atom) { return evaluate(atom, observation); });
	}
	EXPECT_EQ(monitor.finish(), ltl::Verdict::TRUE);
	EXPECT_EQ(monitor.activations(), 2u);
}

TEST(LtlMonitor, ReportsZeroActivationsForAPassThatNeverFired) {
	CompiledProperty property("always (module(1).left_laser_enabled -> module(1).rfid_active)");
	ltl::Monitor     monitor("test", property.formula());

	// The guard never holds, so the property passes without ever having tested anything.
	for (const Observation& observation : {signals(false, false), signals(true, false)}) {
		(void)monitor.observe([&observation](const ltl::Formula& atom) { return evaluate(atom, observation); });
	}
	EXPECT_EQ(monitor.finish(), ltl::Verdict::TRUE);
	EXPECT_TRUE(monitor.has_trigger());
	EXPECT_EQ(monitor.activations(), 0u);
}

TEST(LtlMonitor, CountsANegatedAntecedentThatWouldOtherwiseFoldAway) {
	// `!p -> q` normalizes to `!!p || q`. Folding the double negation leaves no node carrying the
	// implication, which silently reported every such property as never having fired.
	CompiledProperty property("always (!module(1).left_laser_enabled -> module(1).rfid_active)");
	ltl::Monitor     monitor("test", property.formula());
	ASSERT_TRUE(monitor.has_trigger());

	for (const Observation& observation : {signals(true, false), signals(true, true), signals(true, false)}) {
		(void)monitor.observe([&observation](const ltl::Formula& atom) { return evaluate(atom, observation); });
	}
	EXPECT_EQ(monitor.finish(), ltl::Verdict::TRUE);
	EXPECT_EQ(monitor.activations(), 2u);
}

TEST(LtlMonitor, MarksAPropertyWithoutAnImplicationAsCarryingNoTriggerCount) {
	CompiledProperty property("always module(1).rfid_active");
	ltl::Monitor     monitor("test", property.formula());
	EXPECT_FALSE(monitor.has_trigger());
}

TEST(LtlMonitor, SequenceStepsMustMatchOnStrictlyIncreasingObservations) {
	const std::string formula = "sequence { module(1).rfid_active; [0ms, 0ms] module(1).left_laser_enabled; }";

	// Both hold on the same observation. A sequence is not a conjunction, so one tick cannot satisfy
	// two steps: the second step is still waiting when the trace ends.
	EXPECT_EQ(run(formula, {signals(true, true)}).back(), ltl::Verdict::UNRESOLVED);

	// The second step matches on the observation after the first, which is what [0ms, 0ms] means.
	EXPECT_EQ(run(formula, {signals(true, false), signals(false, true)}).back(), ltl::Verdict::TRUE);

	// Matching the first step does not consume the second's window. The unbounded search for a first
	// match never expires, so the trace runs out rather than refuting the sequence.
	EXPECT_EQ(run(formula, {signals(true, true), signals(false, false)}).back(), ltl::Verdict::UNRESOLVED);

	// Bounding the first step measures it from where the property became active, which lets the
	// sequence be refuted outright instead of only running out of trace.
	EXPECT_EQ(run("sequence { [0ms, 0ms] module(1).rfid_active; [0ms, 0ms] module(1).left_laser_enabled; }",
				  {signals(true, true), signals(false, false)})
				  .back(),
			  ltl::Verdict::FALSE);
}

TEST(LtlMonitor, SequenceStepBoundsAreMeasuredFromTheStepBefore) {
	const std::string formula = "sequence { module(1).rfid_active; [0ms, 1ms] module(1).left_laser_enabled; }";

	// One and two observations after the first step are both inside the window.
	EXPECT_EQ(run(formula, {signals(true, false), signals(false, true)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run(formula, {signals(true, false), signals(false, false), signals(false, true)}).back(),
			  ltl::Verdict::TRUE);

	// Three observations after is past the deadline. The first step is unbounded, so the sequence is
	// left undecided rather than refuted: a later trace could still start the whole chain again.
	EXPECT_EQ(
		run(formula, {signals(true, false), signals(false, false), signals(false, false), signals(false, true)}).back(),
		ltl::Verdict::UNRESOLVED);

	// With the first step bounded there is no later start to wait for, so the missed deadline decides.
	EXPECT_EQ(run("sequence { [0ms, 0ms] module(1).rfid_active; [0ms, 1ms] module(1).left_laser_enabled; }",
				  {signals(true, false), signals(false, false), signals(false, false), signals(false, true)})
				  .back(),
			  ltl::Verdict::FALSE);
}

TEST(LtlMonitor, SequenceMatchesTheEquivalentNestedEventuallyStaircase) {
	const std::string sequenced = "sequence { module(1).rfid_active; [0ms, 2ms] module(1).left_laser_enabled;"
								  " [0ms, 2ms] module(1).rfid_active; }";
	const std::string staircase = "eventually (module(1).rfid_active && (next (eventually[0ms, 2ms] "
								  "(module(1).left_laser_enabled && (next (eventually[0ms, 2ms] "
								  "module(1).rfid_active))))))";

	for (std::size_t length = 1; length <= 5; length++) {
		const std::size_t trace_count = std::size_t{1} << (2 * length);
		for (std::size_t bits = 0; bits < trace_count; bits++) {
			std::vector<Observation> trace;
			for (std::size_t tick = 0; tick < length; tick++) {
				trace.push_back(signals((bits & (std::size_t{1} << (2 * tick))) != 0,
										(bits & (std::size_t{1} << (2 * tick + 1))) != 0));
			}
			EXPECT_EQ(run(sequenced, trace).back(), run(staircase, trace).back())
				<< "length=" << length << " bits=" << bits;
		}
	}
}

TEST(LtlMonitor, ImplementsStrongUntil) {
	const std::string formula = "module(1).rfid_active until module(1).left_laser_enabled";

	EXPECT_EQ(run(formula, {signals(false, true)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run(formula, {signals(true, false), signals(true, true)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run(formula, {signals(false, false), signals(true, true)})[0], ltl::Verdict::FALSE);
	// The prefix held throughout, so a longer trace could still have produced the witness.
	EXPECT_EQ(run(formula, {signals(true, false), signals(true, false)}).back(), ltl::Verdict::UNRESOLVED);
}

TEST(LtlMonitor, EventuallyUsesInclusiveMetricBounds) {
	const std::string formula = "eventually[1ms, 2ms] module(1).rfid_active";

	EXPECT_EQ(run(formula, {signals(true), signals(false), signals(false)}).back(), ltl::Verdict::FALSE)
		<< "a witness one tick before the lower bound must not count";
	EXPECT_EQ(run(formula, {signals(false), signals(true), signals(false)}).back(), ltl::Verdict::TRUE)
		<< "the lower endpoint is inclusive";
	EXPECT_EQ(run(formula, {signals(false), signals(false), signals(true)}).back(), ltl::Verdict::TRUE)
		<< "the upper endpoint is inclusive";
	EXPECT_EQ(run(formula, {signals(false), signals(false), signals(false), signals(true)}).back(), ltl::Verdict::FALSE)
		<< "a witness one tick after the upper bound must not count";
	EXPECT_EQ(run(formula, {signals(false), signals(false)}).back(), ltl::Verdict::UNRESOLVED)
		<< "a finite end reached before the deadline leaves the window undecided";
}

TEST(LtlMonitor, AlwaysChecksOnlyTheAvailableClosedMetricInterval) {
	const std::string formula = "always[1ms, 2ms] module(1).rfid_active";

	EXPECT_EQ(run(formula, {signals(false)}).back(), ltl::Verdict::TRUE)
		<< "an early finite end has no positions in the requested interval";
	EXPECT_EQ(run(formula, {signals(false), signals(true), signals(true), signals(false)}).back(), ltl::Verdict::TRUE);
	EXPECT_EQ(run(formula, {signals(true), signals(false), signals(true)}).back(), ltl::Verdict::FALSE);
	EXPECT_EQ(run(formula, {signals(true), signals(true), signals(false)}).back(), ltl::Verdict::FALSE);
}

TEST(LtlMonitor, BoundedUntilRequiresThePrefixEvenBeforeTheLowerBound) {
	const std::string formula = "module(1).rfid_active until[2ms, 3ms] module(1).left_laser_enabled";

	EXPECT_EQ(run(formula, {signals(false, false), signals(true, false), signals(true, true)}).front(),
			  ltl::Verdict::FALSE);
	EXPECT_EQ(run(formula, {signals(true, true), signals(true, false), signals(true, true)}).back(), ltl::Verdict::TRUE)
		<< "a right-hand witness before the lower bound is ignored";
	EXPECT_EQ(
		run(formula, {signals(true, false), signals(true, false), signals(true, false), signals(true, true)}).back(),
		ltl::Verdict::TRUE);
	EXPECT_EQ(
		run(formula, {signals(true, false), signals(true, false), signals(true, false), signals(true, false)}).back(),
		ltl::Verdict::FALSE);
}

TEST(LtlMonitor, ReportsMetricObligationCreationAndDeadlineTicks) {
	CompiledProperty  property("eventually[1ms, 2ms] module(1).rfid_active");
	ltl::Monitor      monitor("test", property.formula(), 10);
	const Observation absent = signals(false);

	EXPECT_EQ(monitor.observe([&](const ltl::Formula& atom) { return evaluate(atom, absent); }), ltl::Verdict::PENDING);
	EXPECT_EQ(monitor.observe([&](const ltl::Formula& atom) { return evaluate(atom, absent); }), ltl::Verdict::PENDING);
	EXPECT_EQ(monitor.observe([&](const ltl::Formula& atom) { return evaluate(atom, absent); }), ltl::Verdict::FALSE);
	ASSERT_TRUE(monitor.failure().has_value());
	EXPECT_EQ(monitor.failure()->obligation_created_tick, 10u);
	EXPECT_EQ(monitor.failure()->deadline_tick, 12u);
	EXPECT_NE(monitor.failure()->formula.find("eventually[1ms, 2ms]"), std::string::npos);
}

TEST(LtlMonitor, KeepsOverlappingRelativeObligationsAtTheirOwnAges) {
	const std::string formula = "always (module(1).rfid_active -> (eventually[1ms, 2ms] module(1).left_laser_enabled))";

	EXPECT_EQ(
		run(formula, {signals(true, false), signals(true, false), signals(false, true), signals(false, false)}).back(),
		ltl::Verdict::TRUE);
	const std::vector<ltl::Verdict> late =
		run(formula, {signals(true, false), signals(true, false), signals(false, false), signals(false, true)});
	EXPECT_EQ(late[2], ltl::Verdict::FALSE)
		<< "the response for the second trigger is too late to repair the first trigger's obligation";
}

TEST(LtlMonitor, OverlappingFailureReportsTheEarliestExpiredObligation) {
	CompiledProperty property("always (module(1).rfid_active -> (eventually[1ms, 2ms] module(1).left_laser_enabled))");
	ltl::Monitor     monitor("test", property.formula(), 10);
	for (const Observation& observation : {signals(true, false), signals(true, false), signals(false, false)}) {
		(void)monitor.observe([&observation](const ltl::Formula& atom) { return evaluate(atom, observation); });
	}

	EXPECT_EQ(monitor.verdict(), ltl::Verdict::FALSE);
	ASSERT_TRUE(monitor.failure().has_value());
	EXPECT_EQ(monitor.failure()->obligation_created_tick, 10u);
	EXPECT_EQ(monitor.failure()->deadline_tick, 12u);
}

TEST(LtlMonitor, BoundedAlwaysActivatesItsNestedMetricOperandAtEachRelativePosition) {
	const std::string formula = "always[1ms, 2ms] (eventually[1ms, 2ms] module(1).left_laser_enabled)";

	EXPECT_EQ(run(formula, {signals(false, false), signals(false, false), signals(false, false), signals(false, true),
							signals(false, false)})
				  .back(),
			  ltl::Verdict::TRUE)
		<< "the tick-3 witness is offset 2 from the tick-1 activation and offset 1 from tick 2";
	EXPECT_EQ(run(formula, {signals(false, false), signals(false, false), signals(false, true), signals(false, false),
							signals(false, false)})
				  .back(),
			  ltl::Verdict::FALSE)
		<< "the tick-2 witness precedes the nested window activated at tick 2";
}

TEST(LtlMonitor, NestedEventuallyStartsItsInnerBoundAtEachCandidateWitness) {
	const std::string formula = "eventually[1ms, 2ms] (always[1ms, 2ms] module(1).left_laser_enabled)";

	EXPECT_EQ(run(formula, {signals(false, false), signals(false, false), signals(false, false), signals(false, true),
							signals(false, true)})
				  .back(),
			  ltl::Verdict::TRUE)
		<< "the successful outer witness is tick 2, so its inner interval is absolute ticks 3 through 4";
	EXPECT_EQ(run(formula, {signals(false, false), signals(false, false), signals(false, true), signals(false, false),
							signals(false, true)})
				  .back(),
			  ltl::Verdict::FALSE);
}

TEST(LtlMonitor, BoundedUntilMayWaitForATemporalRightOperandStartedInsideItsWitnessWindow) {
	const std::string formula =
		"module(1).rfid_active until[1ms, 2ms] (eventually[1ms, 1ms] module(1).left_laser_enabled)";

	EXPECT_EQ(
		run(formula, {signals(true, false), signals(true, false), signals(false, false), signals(false, true)}).back(),
		ltl::Verdict::TRUE)
		<< "the until witness is tick 2; its temporal right operand resolves at tick 3";
	const std::vector<ltl::Verdict> broken_prefix =
		run(formula, {signals(true, false), signals(false, false), signals(false, false), signals(false, true)});
	EXPECT_EQ(broken_prefix[1], ltl::Verdict::PENDING)
		<< "the temporal right-hand candidate started at tick 1 has not resolved yet";
	EXPECT_EQ(broken_prefix[2], ltl::Verdict::FALSE)
		<< "once that candidate fails, the broken prefix forbids the later witness position";
}

TEST(LtlMonitor, NegationPreservesTheNestedRelativeOrigin) {
	const std::string formula = "!(eventually[1ms, 2ms] (eventually[1ms, 1ms] module(1).left_laser_enabled))";

	EXPECT_EQ(
		run(formula, {signals(false, true), signals(false, false), signals(false, false), signals(false, true)}).back(),
		ltl::Verdict::FALSE)
		<< "the tick-0 event is outside both outer candidates, while tick 3 satisfies the inner bound from tick 2";
	EXPECT_EQ(run(formula, {signals(false, true), signals(false, false), signals(false, false), signals(false, false)})
				  .back(),
			  ltl::Verdict::TRUE);
}

TEST(LtlMonitor, AMetricQuietPeriodCanRejectRepeatedEventPulses) {
	const std::string formula = "always (module(1).rfid_active -> (always[1ms, 3ms] !module(1).rfid_active))";

	EXPECT_EQ(run(formula, {signals(true), signals(false), signals(false), signals(true)})[3], ltl::Verdict::FALSE)
		<< "both quiet-period endpoints are inclusive";
	EXPECT_EQ(run(formula, {signals(true), signals(false), signals(false), signals(false), signals(true)}).back(),
			  ltl::Verdict::TRUE)
		<< "an event after the quiet period starts a new independent obligation";
}

TEST(LtlMonitor, AppliesFiniteEndSemanticsThroughBooleanStructure) {
	EXPECT_EQ(run("(always module(1).rfid_active) || (eventually module(1).left_laser_enabled)", {signals(true, false)})
				  .back(),
			  ltl::Verdict::TRUE);
	EXPECT_EQ(run("!(eventually module(1).rfid_active)", {signals(false), signals(false)}).back(), ltl::Verdict::TRUE);
}

TEST(LtlMonitor, AbsorbsRepeatedNestedEventuallyObligations) {
	CompiledProperty property("eventually (module(1).rfid_active && (eventually module(1).left_laser_enabled))");
	ltl::Monitor     monitor("test", property.formula());

	const Observation waiting = signals(true, false);
	for (int tick = 0; tick < 10000; tick++) {
		EXPECT_EQ(monitor.observe([&waiting](const ltl::Formula& atom) { return evaluate(atom, waiting); }),
				  ltl::Verdict::PENDING);
	}

	const Observation witnessed = signals(true, true);
	EXPECT_EQ(monitor.observe([&witnessed](const ltl::Formula& atom) { return evaluate(atom, witnessed); }),
			  ltl::Verdict::TRUE);
}

TEST(LtlMonitor, RejectsAnEmptyFiniteTrace) {
	CompiledProperty property("true");
	ltl::Monitor     monitor("test", property.formula());

	EXPECT_THROW((void)monitor.finish(), std::logic_error);
}

TEST(LtlMonitor, RejectsObservationsAfterFinish) {
	CompiledProperty property("always true");
	ltl::Monitor     monitor("test", property.formula());
	(void)monitor.observe([](const ltl::Formula&) { return true; });
	(void)monitor.finish();

	EXPECT_THROW((void)monitor.observe([](const ltl::Formula&) { return true; }), std::logic_error);
}

TEST(LtlMonitor, MatchesReferenceSemanticsForEveryShortBooleanTrace) {
	const std::vector<std::string> formulas = {
		"module(1).rfid_active",
		"!module(1).rfid_active",
		"module(1).rfid_active && module(1).left_laser_enabled",
		"module(1).rfid_active || module(1).left_laser_enabled",
		"module(1).rfid_active -> module(1).left_laser_enabled",
		"always module(1).rfid_active",
		"eventually module(1).rfid_active",
		"module(1).rfid_active until module(1).left_laser_enabled",
		"eventually[1ms, 2ms] module(1).rfid_active",
		"always[1ms, 2ms] module(1).rfid_active",
		"module(1).rfid_active until[1ms, 2ms] module(1).left_laser_enabled",
		"always (module(1).rfid_active -> (eventually[1ms, 2ms] module(1).left_laser_enabled))",
		"always[0ms, 1ms] (eventually[1ms, 2ms] module(1).rfid_active)",
		"eventually[1ms, 2ms] (always[0ms, 1ms] module(1).left_laser_enabled)",
		"(always[0ms, 1ms] module(1).rfid_active) until[1ms, 2ms] (eventually[0ms, 1ms] module(1).left_laser_enabled)",
		"!(eventually[1ms, 2ms] (always[0ms, 1ms] module(1).rfid_active))",
		"always (module(1).rfid_active -> (always[1ms, 2ms] !module(1).rfid_active))",
		"!(eventually (!module(1).rfid_active && module(1).left_laser_enabled))",
		"always (module(1).rfid_active -> (eventually module(1).left_laser_enabled))",
		"(eventually module(1).rfid_active) until (always module(1).left_laser_enabled)",
		// A trigger that holds on consecutive ticks starts overlapping copies of one timed obligation.
		"always (module(1).rfid_active -> (eventually[0ms, 2ms] module(1).left_laser_enabled))",
		"always (module(1).rfid_active -> (always[0ms, 2ms] module(1).left_laser_enabled))",
		"always (module(1).rfid_active -> (module(1).rfid_active until[0ms, 2ms] module(1).left_laser_enabled))",
		"always (module(1).rfid_active -> (!module(1).left_laser_enabled until[1ms, 3ms] module(1).rfid_active))",
		"always (module(1).rfid_active -> (always[1ms, 3ms] module(1).left_laser_enabled))",
		"always ((eventually[0ms, 1ms] module(1).rfid_active) && ((always[0ms, 2ms] !module(1).left_laser_enabled) "
		"|| (eventually[0ms, 2ms] module(1).left_laser_enabled)))",
	};

	for (const std::string& source : formulas) {
		CompiledProperty property(source);
		for (std::size_t length = 1; length <= 6; length++) {
			const std::size_t trace_count = std::size_t{1} << (2 * length);
			for (std::size_t bits = 0; bits < trace_count; bits++) {
				std::vector<Observation> trace;
				for (std::size_t tick = 0; tick < length; tick++) {
					trace.push_back(signals((bits & (std::size_t{1} << (2 * tick))) != 0,
											(bits & (std::size_t{1} << (2 * tick + 1))) != 0));
				}
				const ltl::Verdict expected =
					reference(property.formula(), trace, 0) ? ltl::Verdict::TRUE : ltl::Verdict::FALSE;
				EXPECT_EQ(two_valued(run(source, trace).back()), expected)
					<< source << " length=" << length << " bits=" << bits;
			}
		}
	}
}

TEST(LtlMonitor, RetainsTheDecisiveObligationAndAtomFacts) {
	CompiledProperty  property("always (module(1).rfid_active -> module(1).left_laser_enabled)");
	ltl::Monitor      monitor("test", property.formula());
	const Observation counterexample = signals(true, false);
	EXPECT_EQ(monitor.observe([&](const ltl::Formula& atom) { return evaluate(atom, counterexample); }),
			  ltl::Verdict::FALSE);
	ASSERT_TRUE(monitor.failure().has_value());
	EXPECT_NE(monitor.failure()->formula.find("always"), std::string::npos);
	EXPECT_NE(monitor.failure()->obligation.find("always"), std::string::npos);
	EXPECT_EQ(monitor.failure()->facts.size(), 2u);
}
