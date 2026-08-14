#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>

#include "../../src/defs.h"
#include "../../src/ltl/checker.h"
#include "../../src/ltl/monitor.h"
#include "../../src/ltl/parser.h"
#include "../../src/ltl/token.h"

namespace {

	ltl::File compile(const std::string& source) {
		ltl::File file = ltl::parse(source, "example.spec");
		ltl::type_check(file, ltl::TypeEnvironment{{1, 2, TOWER_MODULE_ID}}, "example.spec");
		return file;
	}

	std::string error_from(const std::string& source) {
		try {
			(void)compile(source);
		} catch (const std::invalid_argument& error) {
			return error.what();
		}
		return "";
	}

} // namespace

TEST(LtlLexer, RetainsSourcePositions) {
	const std::vector<ltl::Token> tokens = ltl::lex("let m = module(1);\nprop safe = true;", "route.spec");

	ASSERT_GE(tokens.size(), 11u);
	EXPECT_EQ(tokens[0].text, "let");
	EXPECT_EQ(tokens[0].span.begin.line, 1u);
	EXPECT_EQ(tokens[0].span.begin.column, 1u);
	EXPECT_EQ(tokens[8].text, "prop");
	EXPECT_EQ(tokens[8].span.begin.line, 2u);
	EXPECT_EQ(tokens[8].span.begin.column, 1u);
}

TEST(LtlLexer, TreatsByteLiteralsAsSingleSourcePositionedTokens) {
	const std::vector<ltl::Token> tokens = ltl::lex("prop bytes = \"a\\n\\x00b\" == \"\";", "bytes.spec");

	ASSERT_GE(tokens.size(), 8u);
	EXPECT_EQ(tokens[3].kind, ltl::TokenKind::BYTE_LITERAL);
	EXPECT_EQ(tokens[3].text, "\"a\\n\\x00b\"");
	EXPECT_EQ(tokens[3].span.begin.column, 14u);
}

TEST(LtlParser, DecodesByteLiteralsWithoutAStringPrefix) {
	ltl::File file = compile(R"(prop bytes = "hello\n\r\t\"\\\x00\xff" != "";)");

	const ltl::Value& value = *file.properties[0].formula->value;
	ASSERT_EQ(value.kind, ltl::Value::Kind::BYTES);
	const std::vector<uint8_t> expected{'h', 'e', 'l', 'l', 'o', '\n', '\r', '\t', '"', '\\', 0x00, 0xff};
	EXPECT_EQ(value.bytes, expected);
	EXPECT_EQ(value.type, ltl::ValueType::BYTES);
	EXPECT_NO_THROW((void)compile(R"(prop utf8 = "héllo" == "héllo";)"));
}

TEST(LtlParser, ParsesEnumLiteralsGenericallyWithoutKeywordTokens) {
	ltl::File file = ltl::parse("prop status = .ok == .undelivered;", "enum.spec");

	ASSERT_EQ(file.properties[0].formula->value->kind, ltl::Value::Kind::ENUM_LITERAL);
	EXPECT_EQ(file.properties[0].formula->value->text, "ok");
	EXPECT_EQ(file.properties[0].formula->compared->text, "undelivered");
	EXPECT_NE(error_from("prop status = .ok == .invalid;").find("enum literal type cannot be inferred uniquely"),
			  std::string::npos);
}

TEST(LtlParser, ParsesTypedQuantifiersAndCount) {
	ltl::File file = compile(R"(
prop owned = always (
    forall tub: Tub:
        present(tub) -> (
            exists owner: Module:
                at_module(tub, owner)
        )
);
prop occupied = count tub: Tub where (present(tub)) >= 1;
)");

	ASSERT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::ALWAYS);
	const ltl::Formula& universal = *file.properties[0].formula->left;
	ASSERT_EQ(universal.kind, ltl::Formula::Kind::FORALL);
	EXPECT_EQ(universal.binding, "tub");
	EXPECT_EQ(universal.binding_type, ltl::ValueType::TUB);
	ASSERT_EQ(file.properties[1].formula->value->kind, ltl::Value::Kind::COUNT);
	EXPECT_EQ(file.properties[1].formula->value->binding_type, ltl::ValueType::TUB);
}

TEST(LtlLexer, RetainsTlaQuantifierAndCountAliasTokens) {
	const std::vector<ltl::Token> tokens = ltl::lex(R"(\A tub: Tub: \E owner: Module: # tub: Tub)");

	ASSERT_GE(tokens.size(), 14u);
	EXPECT_EQ(tokens[0].kind, ltl::TokenKind::FORALL_ALIAS);
	EXPECT_EQ(tokens[0].text, "\\A");
	EXPECT_EQ(tokens[5].kind, ltl::TokenKind::EXISTS_ALIAS);
	EXPECT_EQ(tokens[5].text, "\\E");
	EXPECT_EQ(tokens[10].kind, ltl::TokenKind::COUNT_ALIAS);
	EXPECT_EQ(tokens[10].text, "#");
}

TEST(LtlParser, AcceptsTlaQuantifierAndCountAliasesButRendersCanonicalNames) {
	ltl::File file = compile(R"(
prop all = \A tub: Tub: !present(tub);
prop any = \E request in rpc_requests(module(1)): request.confirmable;
prop number = # request in rpc_requests(module(1)) where (request.confirmable) >= 0;
prop following = X true;
)");

	ASSERT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::FORALL);
	ASSERT_EQ(file.properties[1].formula->kind, ltl::Formula::Kind::EXISTS);
	ASSERT_EQ(file.properties[2].formula->value->kind, ltl::Value::Kind::COUNT);
	ltl::Monitor all("all", *file.properties[0].formula);
	ltl::Monitor any("any", *file.properties[1].formula);
	ltl::Monitor number("number", *file.properties[2].formula);
	ltl::Monitor following("following", *file.properties[3].formula);
	EXPECT_EQ(all.formula(), "forall tub: Tub: !(present(tub))");
	EXPECT_EQ(any.formula(), "exists request in rpc_requests(module(1)): request.confirmable");
	EXPECT_EQ(number.formula(), "count request in rpc_requests(module(1)) where (request.confirmable) >= 0");
	EXPECT_EQ(following.formula(), "next (true)");
	EXPECT_NE(error_from(R"(let owner = module(1); prop p = \A owner: Module: true;)")
				  .find("bound name 'owner' shadows an alias"),
			  std::string::npos);
}

TEST(LtlLexer, RejectsUnknownBackslashOperators) {
	EXPECT_THROW((void)ltl::lex(R"(\a tub: Tub: true)"), std::invalid_argument);
}

TEST(LtlParser, ParsesExpressionDomainsBeforeTheirObservationAdapterExists) {
	ltl::File file = ltl::parse(R"(
prop requests = forall request in rpc_requests(module(1)): true;
prop responses = count response in rpc_responses(module(1)) where (true) == 0;
)",
								"domains.spec");

	ASSERT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::FORALL);
	ASSERT_TRUE(file.properties[0].formula->domain);
	EXPECT_EQ(file.properties[0].formula->domain->text, "rpc_requests");
	ASSERT_EQ(file.properties[1].formula->value->kind, ltl::Value::Kind::COUNT);
	EXPECT_EQ(file.properties[1].formula->value->domain->text, "rpc_responses");
}

TEST(LtlLexer, RejectsMalformedByteLiterals) {
	EXPECT_NE(error_from(R"(prop p = "unterminated;)").find("unterminated byte literal"), std::string::npos);
	EXPECT_NE(error_from(R"(prop p = "line
break" == "";)")
				  .find("newline in byte literal"),
			  std::string::npos);
	EXPECT_NE(error_from(R"(prop p = "\q" == "";)").find("unknown byte escape"), std::string::npos);
	EXPECT_NE(error_from(R"(prop p = "\x0" == "";)").find("requires exactly two hexadecimal digits"),
			  std::string::npos);
}

TEST(LtlParser, ParsesAliasesFieldsAndPredicates) {
	ltl::File file = compile(R"(
let m = module(1);
let t = tub(7);
	prop route = always (
	    at_module(t, m)
	    && m.left_laser_enabled
	    && at_position(t, m.right_laser)
	    && m.led_color != 0xff0000
	);
)");

	ASSERT_EQ(file.properties.size(), 1u);
	EXPECT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::ALWAYS);
	const ltl::Formula& at_module = *file.properties[0].formula->left->left->left->left;
	ASSERT_EQ(at_module.value->kind, ltl::Value::Kind::CALL);
	EXPECT_EQ(at_module.value->arguments[0]->entity_id, 7u);
	EXPECT_EQ(at_module.value->arguments[1]->entity_id, 1u);
}

TEST(LtlParser, ExpandsGeneralAliasesAsGroupedSyntax) {
	ltl::File file = compile(R"(
let m = module(1);
let commands_changed =
    changed(m.led_color)
    || changed(m.servo_angle)
    || changed(m.small_belt_speed)
    || changed(m.big_belt_speed);
let commanded = commands_changed && m.left_laser_enabled;
prop response = always (commanded -> commands_changed);
)");

	const ltl::Formula& implication = *file.properties[0].formula->left;
	ASSERT_EQ(implication.kind, ltl::Formula::Kind::IMPLIES);
	EXPECT_EQ(implication.left->kind, ltl::Formula::Kind::AND);
	EXPECT_EQ(implication.left->left->kind, ltl::Formula::Kind::OR);
	EXPECT_EQ(implication.right->kind, ltl::Formula::Kind::OR);
}

TEST(LtlParser, AllowsArbitrarilyParenthesizedValues) {
	EXPECT_NO_THROW((void)compile("prop p = (((module(1)))).led_color == 0x000000;"));
	EXPECT_NO_THROW((void)compile("prop p = changed(((((module(1)))).led_color));"));
	EXPECT_NE(error_from("prop p = (module(1)).unknown;").find("unknown Module member 'unknown'"), std::string::npos);
	EXPECT_NE(error_from("prop p = (unknown()).foo;").find("unknown function 'unknown'"), std::string::npos);
}

TEST(LtlParser, RejectsInvalidAliasReferencesAndNames) {
	EXPECT_NE(error_from("let recursive = recursive; prop p = true;").find("recursive alias 'recursive'"),
			  std::string::npos);
	EXPECT_NE(error_from("let first = second; let second = true; prop p = first;")
				  .find("cannot be referenced before its declaration"),
			  std::string::npos);
	EXPECT_NE(error_from("let CommandsChanged = true; prop p = true;").find("must use snake_case"), std::string::npos);
}

TEST(LtlParser, AllowsHomogeneousBooleanChains) {
	ltl::File file = compile("prop p = true && false && true;");

	const ltl::Formula& root = *file.properties[0].formula;
	ASSERT_EQ(root.kind, ltl::Formula::Kind::AND);
	EXPECT_EQ(root.left->kind, ltl::Formula::Kind::AND);
}

TEST(LtlParser, UsesParenthesesInsteadOfBinaryPrecedence) {
	ltl::File file = compile("prop p = true until (false && true);");

	const ltl::Formula& root = *file.properties[0].formula;
	ASSERT_EQ(root.kind, ltl::Formula::Kind::UNTIL);
	EXPECT_EQ(root.right->kind, ltl::Formula::Kind::AND);
}

TEST(LtlParser, AcceptsCanonicalAndSymbolicOperators) {
	ltl::File file = compile("prop p = (G (!false && (F true))) || (true U false);");

	EXPECT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::OR);
}

TEST(LtlParser, ParsesCanonicalAndSymbolicStrongNext) {
	ltl::File file = compile("prop p = (next true) && (X false);");

	ASSERT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::AND);
	EXPECT_EQ(file.properties[0].formula->left->kind, ltl::Formula::Kind::NEXT);
	EXPECT_EQ(file.properties[0].formula->right->kind, ltl::Formula::Kind::NEXT);
	EXPECT_NO_THROW((void)compile("prop p = !X false;"));
	EXPECT_NE(error_from("prop p = X[1ms, 1ms] true;").find("next does not accept a metric interval"),
			  std::string::npos);
}

TEST(LtlParser, RejectsMixedBinaryOperatorsWithoutGrouping) {
	EXPECT_NE(error_from("prop p = true || false && true;").find("require explicit parentheses"), std::string::npos);
	EXPECT_NE(error_from("prop p = true until false && true;").find("require explicit parentheses"), std::string::npos);
}

TEST(LtlParser, RejectsNonAssociativeChainsWithoutGrouping) {
	EXPECT_NE(error_from("prop p = true -> false -> true;").find("require explicit parentheses"), std::string::npos);
	EXPECT_NE(error_from("prop p = true U false U true;").find("require explicit parentheses"), std::string::npos);
}

TEST(LtlParser, RequiresGroupingWhenTemporalAndOtherOperatorsMeet) {
	EXPECT_NE(error_from("prop p = always true && false;").find("temporal operators require parentheses"),
			  std::string::npos);
	EXPECT_NE(error_from("prop p = true -> eventually false;").find("temporal operators require parentheses"),
			  std::string::npos);

	EXPECT_NO_THROW((void)compile("prop p = always (true && false);"));
	EXPECT_NO_THROW((void)compile("prop p = (always true) && false;"));
	EXPECT_NO_THROW((void)compile("prop p = true -> (eventually false);"));
}

TEST(LtlParser, RequiresGroupingBetweenTemporalOperators) {
	EXPECT_NE(error_from("prop p = always eventually true;").find("require explicit parentheses"), std::string::npos);
	EXPECT_NE(error_from("prop p = always X true;").find("require explicit parentheses"), std::string::npos);

	EXPECT_NO_THROW((void)compile("prop p = always (eventually true);"));
	EXPECT_NO_THROW((void)compile("prop p = always (X true);"));
}

TEST(LtlParser, BangDirectlyNegatesTemporalOperators) {
	ltl::File file = compile("prop p = !eventually (true && false);");

	const ltl::Formula& negation = *file.properties[0].formula;
	ASSERT_EQ(negation.kind, ltl::Formula::Kind::NOT);
	ASSERT_EQ(negation.left->kind, ltl::Formula::Kind::EVENTUALLY);
	EXPECT_EQ(negation.left->left->kind, ltl::Formula::Kind::AND);
	EXPECT_NO_THROW((void)compile("prop p = !G true;"));
	EXPECT_NO_THROW((void)compile("prop p = !!always true;"));
}

TEST(LtlParser, BangIsTheOnlyPrecedenceBearingLogicalOperator) {
	ltl::File file = compile("prop p = !true && false;");

	ASSERT_EQ(file.properties[0].formula->kind, ltl::Formula::Kind::AND);
	EXPECT_EQ(file.properties[0].formula->left->kind, ltl::Formula::Kind::NOT);
}

TEST(LtlParser, RejectsBooleanWordOperators) {
	EXPECT_NE(error_from("prop p = not true;").find("use '!'"), std::string::npos);
	EXPECT_NE(error_from("prop p = true and false;").find("use '&&'"), std::string::npos);
	EXPECT_NE(error_from("prop p = true or false;").find("use '||'"), std::string::npos);
}

TEST(LtlParser, ParsesSignedNumericComparisons) {
	ltl::File file = compile("prop reverse = module(1).big_belt_speed < -50.5;");

	const ltl::Formula& formula = *file.properties[0].formula;
	ASSERT_EQ(formula.kind, ltl::Formula::Kind::COMPARISON);
	EXPECT_DOUBLE_EQ(formula.compared->number, -50.5);
}

TEST(LtlChecker, RejectsUnknownNamesWithLocation) {
	const std::string error = error_from("prop p = present(missing);");

	EXPECT_NE(error.find("example.spec:1:18"), std::string::npos);
	EXPECT_NE(error.find("unknown name 'missing'"), std::string::npos);
}

TEST(LtlChecker, RejectsUnknownModuleFields) {
	const std::string error = error_from("prop p = module(1).secret;");

	EXPECT_NE(error.find("unknown Module member 'secret'"), std::string::npos);
}

TEST(LtlChecker, RejectsIncompatibleComparisons) {
	const std::string error = error_from("prop p = module(1).led_color == 12;");

	EXPECT_NE(error.find("cannot compare Color with Number"), std::string::npos);
}

TEST(LtlChecker, RestrictsOrderedComparisonToNumbers) {
	const std::string error = error_from("prop p = module(1).led_color < 0xff0000;");

	EXPECT_NE(error.find("ordered comparison requires numeric operands"), std::string::npos);
}

TEST(LtlChecker, RestrictsByteValuesToEqualityComparisons) {
	EXPECT_NO_THROW((void)compile(R"(prop p = "\x00\xff" == "\x00\xff";)"));
	EXPECT_NO_THROW((void)compile(R"(prop p = "left" != "right";)"));
	EXPECT_NE(error_from(R"(prop p = "left" < "right";)").find("ordered comparison requires numeric operands"),
			  std::string::npos);
	EXPECT_NE(error_from(R"(prop p = "12" == 12;)").find("cannot compare Bytes with Number"), std::string::npos);
}

TEST(LtlChecker, EnforcesQuantifierBindingScopeAndNames) {
	EXPECT_NE(error_from("prop p = forall BadName: Tub: present(BadName);").find("must use snake_case"),
			  std::string::npos);
	EXPECT_NE(error_from("prop p = (forall tub: Tub: present(tub)) && present(tub);").find("unknown name 'tub'"),
			  std::string::npos);
	EXPECT_NE(
		error_from("prop p = forall tub: Tub: (exists tub: Tub: present(tub));").find("shadows an existing binding"),
		std::string::npos);
	EXPECT_NE(
		error_from("let owner = module(1); prop p = forall owner: Module: owner.rfid_active;").find("shadows an alias"),
		std::string::npos);
}

TEST(LtlChecker, ChecksRpcExpressionDomainsRecordsAndEnums) {
	EXPECT_NE(error_from("prop p = count tub: Tub where (eventually present(tub)) >= 1;")
				  .find("cannot contain temporal operators"),
			  std::string::npos);
	EXPECT_NO_THROW((void)compile(R"(
prop p = forall request in rpc_requests(module(1)):
    request.cmd == .rpc_cmd_laser_left_detect
    -> (rpc_response_to(request).status == .ok);
)"));
	EXPECT_NO_THROW((void)compile(R"(
prop p = count event in rpc_events(module(1)) where (
    event.kind == .rpc_event_message_received && event.payload == "hello"
) >= 1;
)"));
	EXPECT_NE(error_from("prop p = forall request in rpc_requests(tub(1)): true;").find("expects a Module"),
			  std::string::npos);
	EXPECT_NE(error_from("prop p = forall event in rpc_events(module(1)): event.unknown == 1;")
				  .find("unknown RpcEvent member"),
			  std::string::npos);
	EXPECT_NE(error_from("prop p = forall event in rpc_events(module(1)): event.kind == .rpc_event_typo;")
				  .find("unknown RpcEventKind literal"),
			  std::string::npos);
}

TEST(LtlChecker, RejectsInvalidPredicateArguments) {
	const std::string error = error_from("prop p = at_module(module(1), tub(1));");

	EXPECT_NE(error.find("at_module expects a Tub first"), std::string::npos);
}

TEST(LtlChecker, RejectsOutOfRangeEntities) {
	EXPECT_NE(error_from("prop p = present(tub(0));").find("tub id must be between 1 and 255"), std::string::npos);
	EXPECT_NE(error_from("prop p = module(25).rfid_active;").find("module id must be between 1 and 24"),
			  std::string::npos);
}

TEST(LtlChecker, RejectsModulesMissingFromTheLayout) {
	EXPECT_NE(error_from("prop p = module(8).rfid_active;").find("module 8 does not exist in the selected layout"),
			  std::string::npos);
}

TEST(LtlChecker, AcceptsEdgePredicates) {
	EXPECT_NO_THROW((void)compile(R"(
let t = tub(7);
let m = module(1);
prop edges = appeared(t) || disappeared(t) || entered(t, m.entry) || left(t, m.entry)
                 || changed(m.led_color);
)"));
}

TEST(LtlChecker, RejectsChangedOnRpcRecordMembers) {
	const std::string error = error_from(R"(
prop p = forall request in rpc_requests(module(1)):
    changed(request.cmd);
)");

	EXPECT_NE(error.find("changed expects a module output field"), std::string::npos);
}

TEST(LtlChecker, RejectsDuplicateDeclarations) {
	const std::string error = error_from("let item = tub(1); prop item = present(item);");

	EXPECT_NE(error.find("duplicate declaration 'item'"), std::string::npos);
}

TEST(LtlParser, RejectsMalformedRgbColours) {
	const std::string error = error_from("prop p = module(1).led_color == 0xff00;");

	EXPECT_NE(error.find("RGB colours must contain exactly six hexadecimal digits"), std::string::npos);
}

TEST(LtlParser, RequiresSemicolons) {
	const std::string error = error_from("prop p = true");

	EXPECT_NE(error.find("expected ';' after prop declaration"), std::string::npos);
}

TEST(LtlLexer, IgnoresLineCommentsAndRetainsFollowingSourcePositions) {
	ltl::File file = compile(R"(
// A whole disabled proposition may contain syntax the current language does not understand:
// prop future = always (sample(m.power) in [0.6, 1.25]);
let m = module(1); // aliases may also have trailing comments
prop enabled = m.left_laser_enabled;
)");

	ASSERT_EQ(file.properties.size(), 1u);
	EXPECT_EQ(file.properties[0].name, "enabled");
	EXPECT_EQ(file.properties[0].span.begin.line, 5u);
}

TEST(LtlParser, RejectsTheRemovedPropertyKeyword) {
	EXPECT_NE(error_from("property old = true;").find("expected 'let' or 'prop' declaration"), std::string::npos);
}

TEST(LtlParser, AcceptsAnEmptyPropertyFile) {
	ltl::File file = ltl::parse("", "empty.spec");
	EXPECT_TRUE(file.properties.empty());
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 0}, "empty.spec"));
}

TEST(LtlParser, ParsesExactAndInclusivePropertyScopes) {
	ltl::File file = ltl::parse(R"(
prop instant at 1.5s = eventually true;
prop window during [250ms, 2s] = always true;
prop composite at 1h2m3.004s = true;
prop milliseconds at 1.234s = true;
)",
								"scope.spec");
	ltl::type_check(file, ltl::TypeEnvironment{{1}, 4000000}, "scope.spec");

	ASSERT_TRUE(file.properties[0].scope.has_value());
	EXPECT_EQ(file.properties[0].scope->kind, ltl::PropertyScope::Kind::AT);
	EXPECT_EQ(file.properties[0].scope->from_tick, 1500u);
	EXPECT_EQ(file.properties[0].scope->until_tick, 1500u);
	ASSERT_TRUE(file.properties[1].scope.has_value());
	EXPECT_EQ(file.properties[1].scope->kind, ltl::PropertyScope::Kind::DURING);
	EXPECT_EQ(file.properties[1].scope->from_tick, 250u);
	EXPECT_EQ(file.properties[1].scope->until_tick, 2000u);
	ASSERT_TRUE(file.properties[2].scope.has_value());
	EXPECT_EQ(file.properties[2].scope->from_tick, 3723004u);
	ASSERT_TRUE(file.properties[3].scope.has_value());
	EXPECT_EQ(file.properties[3].scope->from_tick, 1234u);
}

TEST(LtlParser, RejectsInexactOrMalformedDurations) {
	EXPECT_NE(error_from("prop p at 0.5ms = true;").find("cannot be represented exactly"), std::string::npos);
	EXPECT_NE(error_from("prop p at 1.0005s = true;").find("cannot be represented exactly"), std::string::npos);
	EXPECT_NE(error_from("prop p at 1 = true;").find("expected 'h', 'm', 's', or 'ms'"), std::string::npos);
	EXPECT_NE(error_from("prop p at 1min = true;").find("duration unit must be 'h', 'm', 's', or 'ms'"),
			  std::string::npos);
}

TEST(LtlParser, EnforcesCanonicalCompositeDurationOrder) {
	EXPECT_NE(error_from("prop p at 1s2m = true;").find("descending order"), std::string::npos);
	EXPECT_NE(error_from("prop p at 1m2m = true;").find("descending order"), std::string::npos);
	EXPECT_NE(error_from("prop p at 1h 2m = true;").find("without whitespace"), std::string::npos);
	EXPECT_NE(error_from("prop p at 1.5h30m = true;").find("only the final duration component"), std::string::npos);
}

TEST(LtlChecker, RejectsReversedPropertyScopeBoundsButAcceptsScopesBeyondTheScenario) {
	EXPECT_NE(error_from("prop p during [2s, 1s] = true;").find("ends before it starts"), std::string::npos);

	ltl::File file = ltl::parse("prop p during [1s, 2001ms] = true;", "scope.spec");
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 2000}, "scope.spec"));
}

TEST(LtlParser, ParsesMetricIntervalsOnEveryTemporalOperator) {
	ltl::File file = ltl::parse(R"(
prop future = eventually[250ms, 2s] true;
prop invariant = G[1.5s, 2m] true;
prop ordered = true until[0s, 1h2m3.004s] false;
)",
								"metric.spec");

	ASSERT_TRUE(file.properties[0].formula->interval.has_value());
	EXPECT_EQ(file.properties[0].formula->interval->from_tick, 250u);
	EXPECT_EQ(file.properties[0].formula->interval->until_tick, 2000u);
	ASSERT_TRUE(file.properties[1].formula->interval.has_value());
	EXPECT_EQ(file.properties[1].formula->interval->from_tick, 1500u);
	EXPECT_EQ(file.properties[1].formula->interval->until_tick, 120000u);
	ASSERT_TRUE(file.properties[2].formula->interval.has_value());
	EXPECT_EQ(file.properties[2].formula->interval->from_tick, 0u);
	EXPECT_EQ(file.properties[2].formula->interval->until_tick, 3723004u);
}

TEST(LtlParser, RejectsMalformedMetricIntervals) {
	EXPECT_NE(error_from("prop p = eventually[2s, 1s] true;").find("ends before it starts"), std::string::npos);
	EXPECT_NE(error_from("prop p = always[0.5ms, 1s] true;").find("cannot be represented exactly"), std::string::npos);
	EXPECT_NE(error_from("prop p = true until[0s 1s] false;").find("without whitespace"), std::string::npos);
}

TEST(LtlParser, ParsesASequenceWhoseStepSemicolonsDoNotEndTheDeclaration) {
	const ltl::File file = compile(R"(
		let bright = module(1).led_color == 0x00ff00;
		prop ordered = sequence {
			module(1).rfid_active;
			[0ms, 250ms] bright;
			[10ms, 250ms] module(1).left_laser_enabled;
		};
		prop after = always module(1).rfid_active;
	)");
	EXPECT_EQ(file.properties.size(), 2u);
	EXPECT_EQ(file.properties[0].name, "ordered");
	EXPECT_EQ(file.properties[1].name, "after");

	// Each step is searched for from one observation after the previous match, so the expansion
	// alternates eventually and next, and the ordering next is not module-relative.
	const ltl::Formula& sequenced = *file.properties[0].formula;
	ASSERT_EQ(sequenced.kind, ltl::Formula::Kind::EVENTUALLY);
	EXPECT_FALSE(sequenced.interval.has_value());
	const ltl::Formula& ordering = *sequenced.left->right;
	ASSERT_EQ(ordering.kind, ltl::Formula::Kind::NEXT);
	EXPECT_TRUE(ordering.sequencing);
	EXPECT_EQ(ordering.step_module, 0u);
	ASSERT_TRUE(ordering.left->interval.has_value());
	EXPECT_EQ(ordering.left->interval->until_tick, 250u);
}

TEST(LtlParser, RejectsMalformedSequences) {
	EXPECT_NE(error_from("prop p = sequence { };").find("at least one step"), std::string::npos);
	EXPECT_NE(error_from("prop p = sequence { true };").find("expected ';' after a sequence step"), std::string::npos);
	EXPECT_NE(error_from("prop p = sequence true;").find("expected '{' after 'sequence'"), std::string::npos);
}

TEST(LtlParser, ComposesASequenceWithOtherOperatorsWithoutExtraParentheses) {
	EXPECT_NO_THROW((void)compile("prop p = !sequence { module(1).rfid_active; [0ms, 1ms] module(2).rfid_active; };"));
	EXPECT_NO_THROW((void)compile("prop p = always (sequence { module(1).rfid_active; [0ms, 1ms] true; });"));
}

TEST(LtlParser, ResolvesTheModuleWhoseStepsANextAdvancesOn) {
	const ltl::File single = compile("prop p = next module(2).rfid_active;");
	EXPECT_EQ(single.properties[0].formula->step_module, 2u);

	// A member access, a count domain, and an alias all reach the same module reference.
	const ltl::File counted = compile("prop p = next (count r in rpc_requests(module(2)) where (r.confirmable) >= 1);");
	EXPECT_EQ(counted.properties[0].formula->step_module, 2u);

	// A formula naming no module follows the global observation tick.
	const ltl::File global = compile("prop p = next present(tub(1));");
	EXPECT_EQ(global.properties[0].formula->step_module, 0u);
}

TEST(LtlParser, RejectsANextThatRefersToMoreThanOneModule) {
	EXPECT_NE(error_from("prop p = next (module(1).rfid_active && module(2).rfid_active);")
				  .find("next refers to more than one module"),
			  std::string::npos);
}

TEST(LtlParser, RejectsANextWhoseModuleIsBoundInsideItsOwnScope) {
	EXPECT_NE(error_from("prop p = next (forall m: Module: m.rfid_active);")
				  .find("next cannot resolve a module bound inside its own scope"),
			  std::string::npos);
}

TEST(LtlParser, CarriesAnEnclosingQuantifiersModuleIntoNext) {
	const ltl::File     file       = compile("prop p = forall m: Module: (next m.rfid_active);");
	const ltl::Formula& quantified = *file.properties[0].formula;
	EXPECT_EQ(quantified.left->step_module, 0u);
	EXPECT_EQ(quantified.left->step_module_binding, "m");
}

TEST(LtlParser, CompilesTheHighLevelHealthyHardwareScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/hardware_test.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 116000}, path));
	EXPECT_EQ(file.properties.size(), 22u);
}

TEST(LtlParser, CompilesTheHighLevelModuleMovementScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/module_movement.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 45000}, path));
	EXPECT_EQ(file.properties.size(), 25u);
}

TEST(LtlParser, CompilesTheModuleMovementIdleScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/module_movement_idle.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 15000}, path));
	EXPECT_EQ(file.properties.size(), 13u);
}

TEST(LtlParser, CompilesTheBlockedBeamDiagnosticScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/hardware_test_blocked_beam.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 6000}, path));
	EXPECT_EQ(file.properties.size(), 10u);
}

TEST(LtlParser, CompilesTheStartupTagDiagnosticScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/hardware_test_tag_at_startup.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 7000}, path));
	EXPECT_EQ(file.properties.size(), 10u);
}

TEST(LtlParser, CompilesTheLaserAlignmentScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/laser_alignment.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{1}, 16000}, path));
	EXPECT_EQ(file.properties.size(), 13u);
}

TEST(LtlParser, CompilesThePerModuleVariationScenario) {
	const std::string path = std::string(REPO_ROOT) + "/scenarios/per_module_variation.spec";
	std::ifstream     input(path);
	ASSERT_TRUE(input.is_open());
	std::ostringstream source;
	source << input.rdbuf();

	ltl::File file = ltl::parse(source.str(), path);
	EXPECT_NO_THROW(ltl::type_check(file, ltl::TypeEnvironment{{5, 12}, 30000}, path));
	EXPECT_EQ(file.properties.size(), 9u);
}
