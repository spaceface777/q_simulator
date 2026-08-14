#include "parser.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "token.h"

namespace ltl {

	namespace {

		class MacroExpander {
		private:
			using Expansion = std::vector<Token>;

			const std::vector<Token>&                    tokens_;
			std::string                                  filename_;
			std::unordered_map<std::string, std::size_t> declaration_order_;
			std::unordered_map<std::string, Expansion>   expansions_;

			[[noreturn]] void error(const Token& token, const std::string& message) const {
				throw std::invalid_argument(filename_ + ":" + std::to_string(token.span.begin.line) + ":" +
											std::to_string(token.span.begin.column) + ": " + message);
			}

			static bool word(const Token& token, const std::string& text) {
				return token.kind == TokenKind::IDENTIFIER && token.text == text;
			}

			static bool binder(const Token& token) {
				return word(token, "forall") || word(token, "exists") || word(token, "count") ||
					   token.kind == TokenKind::FORALL_ALIAS || token.kind == TokenKind::EXISTS_ALIAS ||
					   token.kind == TokenKind::COUNT_ALIAS;
			}

			std::size_t declaration_end(std::size_t begin) const {
				// A `sequence` block separates its steps with semicolons, so only a semicolon outside
				// every brace ends the declaration.
				std::size_t depth = 0;
				for (std::size_t index = begin; tokens_[index].kind != TokenKind::END; index++) {
					if (tokens_[index].kind == TokenKind::LEFT_BRACE) depth++;
					else if (tokens_[index].kind == TokenKind::RIGHT_BRACE && depth > 0) depth--;
					else if (tokens_[index].kind == TokenKind::SEMICOLON && depth == 0) return index;
				}
				error(tokens_[begin], word(tokens_[begin], "let") ? "expected ';' after alias declaration"
																  : "expected ';' after prop declaration");
			}

			static bool snake_case(const std::string& name) {
				if (name.empty() || (name.front() != '_' && (name.front() < 'a' || name.front() > 'z'))) return false;
				for (const char character : name) {
					if (character != '_' && (character < 'a' || character > 'z') &&
						(character < '0' || character > '9')) {
						return false;
					}
				}
				return true;
			}

			void discover_declarations() {
				std::unordered_set<std::string> names;
				std::size_t                     let_order = 0;
				for (std::size_t index = 0; tokens_[index].kind != TokenKind::END;) {
					const bool let = word(tokens_[index], "let");
					if (!let && !word(tokens_[index], "prop")) {
						error(tokens_[index], "expected 'let' or 'prop' declaration");
					}
					if (tokens_[index + 1].kind != TokenKind::IDENTIFIER) {
						error(tokens_[index + 1],
							  let ? "expected an alias name after 'let'" : "expected a proposition name after 'prop'");
					}
					const Token& name = tokens_[index + 1];
					if (!names.insert(name.text).second) error(name, "duplicate declaration '" + name.text + "'");
					if (let) {
						if (!snake_case(name.text)) error(name, "alias names must use snake_case");
						declaration_order_.emplace(name.text, let_order++);
					}
					index = declaration_end(index) + 1;
				}
			}

			Expansion expand(std::size_t begin, std::size_t end, const std::string* defining,
							 std::size_t available_order) const {
				Expansion result;
				for (std::size_t index = begin; index < end; index++) {
					const Token& token = tokens_[index];
					if (binder(token) && index + 1 < end && tokens_[index + 1].kind == TokenKind::IDENTIFIER &&
						declaration_order_.contains(tokens_[index + 1].text)) {
						error(tokens_[index + 1], "bound name '" + tokens_[index + 1].text + "' shadows an alias");
					}
					const bool member = index > begin && tokens_[index - 1].kind == TokenKind::DOT;
					if (token.kind != TokenKind::IDENTIFIER || member) {
						result.push_back(token);
						continue;
					}

					if (defining && token.text == *defining) {
						error(token, "recursive alias '" + token.text + "' is not allowed");
					}
					const auto declared = declaration_order_.find(token.text);
					if (declared != declaration_order_.end() && declared->second >= available_order) {
						error(token, "alias '" + token.text + "' cannot be referenced before its declaration");
					}
					const auto expansion = expansions_.find(token.text);
					if (expansion == expansions_.end()) {
						result.push_back(token);
						continue;
					}

					result.push_back(Token{TokenKind::LEFT_PAREN, "(", token.span});
					result.insert(result.end(), expansion->second.begin(), expansion->second.end());
					result.push_back(Token{TokenKind::RIGHT_PAREN, ")", token.span});
				}
				return result;
			}

		public:
			MacroExpander(const std::vector<Token>& tokens, std::string filename)
				: tokens_(tokens), filename_(std::move(filename)) {}

			std::vector<Token> run() {
				discover_declarations();
				std::vector<Token> result;
				std::size_t        available_order = 0;
				for (std::size_t index = 0; tokens_[index].kind != TokenKind::END;) {
					const std::size_t end = declaration_end(index);
					if (word(tokens_[index], "let")) {
						const Token& name = tokens_[index + 1];
						if (tokens_[index + 2].kind != TokenKind::EQUAL) {
							error(tokens_[index + 2], "expected '=' after alias name");
						}
						if (index + 3 == end) error(tokens_[end], "expected an alias expansion after '='");
						Expansion body = expand(index + 3, end, &name.text, available_order);
						expansions_.emplace(name.text, std::move(body));
						available_order++;
					} else {
						std::size_t equal = index + 2;
						while (equal < end && tokens_[equal].kind != TokenKind::EQUAL) equal++;
						if (equal == end) error(tokens_[end], "expected '=' after proposition name");
						result.insert(result.end(), tokens_.begin() + static_cast<std::ptrdiff_t>(index),
									  tokens_.begin() + static_cast<std::ptrdiff_t>(equal + 1));
						Expansion body = expand(equal + 1, end, nullptr, available_order);
						result.insert(result.end(), body.begin(), body.end());
						result.push_back(tokens_[end]);
					}
					index = end + 1;
				}
				result.push_back(tokens_.back());
				return result;
			}
		};

		class Parser {
		private:
			std::vector<Token> tokens_;
			std::string        filename_;
			std::size_t        current_ = 0;

			[[nodiscard]] const Token& current() const noexcept { return tokens_[current_]; }

			[[nodiscard]] const Token& previous() const noexcept { return tokens_[current_ - 1]; }

			[[nodiscard]] bool at(TokenKind kind) const noexcept { return current().kind == kind; }

			[[nodiscard]] bool word(const std::string& text) const noexcept {
				return at(TokenKind::IDENTIFIER) && current().text == text;
			}

			[[nodiscard]] bool at_quantifier() const noexcept {
				return word("forall") || word("exists") || at(TokenKind::FORALL_ALIAS) || at(TokenKind::EXISTS_ALIAS);
			}

			bool take(TokenKind kind) {
				if (!at(kind)) return false;
				current_++;
				return true;
			}

			bool take_word(const std::string& text) {
				if (!word(text)) return false;
				current_++;
				return true;
			}

			[[noreturn]] void error(const Token& token, const std::string& message) const {
				throw std::invalid_argument(filename_ + ":" + std::to_string(token.span.begin.line) + ":" +
											std::to_string(token.span.begin.column) + ": " + message);
			}

			[[noreturn]] void error(const Span& span, const std::string& message) const {
				throw std::invalid_argument(filename_ + ":" + std::to_string(span.begin.line) + ":" +
											std::to_string(span.begin.column) + ": " + message);
			}

			const Token& require(TokenKind kind, const std::string& message) {
				if (!take(kind)) error(current(), message);
				return previous();
			}

			const Token& require_name(const std::string& message) {
				if (!take(TokenKind::IDENTIFIER)) error(current(), message);
				return previous();
			}

			struct DurationUnit {
				uint64_t factor;
				int      order;
			};

			DurationUnit duration_unit(const Token& unit) const {
				if (unit.text == "h") return {3600000, 3};
				if (unit.text == "m") return {60000, 2};
				if (unit.text == "s") return {1000, 1};
				if (unit.text == "ms") return {1, 0};
				error(unit, "duration unit must be 'h', 'm', 's', or 'ms'");
			}

			uint64_t duration_component(const Token& number, uint64_t factor) const {
				const std::size_t point    = number.text.find('.');
				const std::string whole    = number.text.substr(0, point);
				std::string       fraction = point == std::string::npos ? "" : number.text.substr(point + 1);

				uint64_t ticks = 0;
				for (const char digit : whole) {
					const uint64_t value = static_cast<uint64_t>(digit - '0');
					if (ticks > (std::numeric_limits<uint64_t>::max() - value) / 10) {
						error(number, "duration is too large");
					}
					ticks = ticks * 10 + value;
				}
				if (ticks > std::numeric_limits<uint64_t>::max() / factor) error(number, "duration is too large");
				ticks *= factor;

				while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
				if (fraction.empty()) return ticks;
				// No supported unit can turn more than seven significant decimal places into whole milliseconds.
				if (fraction.size() > 7) {
					error(number, "duration cannot be represented exactly at 1 ms tick resolution");
				}

				uint64_t numerator   = 0;
				uint64_t denominator = 1;
				for (const char digit : fraction) {
					numerator = numerator * 10 + static_cast<uint64_t>(digit - '0');
					denominator *= 10;
				}
				const uint64_t scaled = numerator * factor;
				if (scaled % denominator != 0) {
					error(number, "duration cannot be represented exactly at 1 ms tick resolution");
				}
				const uint64_t fractional_ticks = scaled / denominator;
				if (ticks > std::numeric_limits<uint64_t>::max() - fractional_ticks) {
					error(number, "duration is too large");
				}
				return ticks + fractional_ticks;
			}

			uint64_t duration() {
				uint64_t ticks          = 0;
				int      previous_order = 4;
				do {
					const Token& number =
						require(TokenKind::NUMBER, "expected a duration with an 'h', 'm', 's', or 'ms' unit");
					const Token&       unit = require_name("expected 'h', 'm', 's', or 'ms' after duration component");
					const DurationUnit parsed_unit = duration_unit(unit);
					if (parsed_unit.order >= previous_order) {
						error(unit, "duration units must appear once in descending order: h, m, s, ms");
					}

					const bool     fractional = number.text.find('.') != std::string::npos;
					const uint64_t component  = duration_component(number, parsed_unit.factor);
					if (ticks > std::numeric_limits<uint64_t>::max() - component) {
						error(number, "duration is too large");
					}
					ticks += component;
					previous_order = parsed_unit.order;
					if (at(TokenKind::NUMBER) && unit.span.end.offset != current().span.begin.offset) {
						error(current(), "duration components must be concatenated without whitespace");
					}
					if (fractional && at(TokenKind::NUMBER)) {
						error(number, "only the final duration component may be fractional");
					}
				} while (at(TokenKind::NUMBER));
				return ticks;
			}

			PropertyScope property_scope() {
				PropertyScope  scope;
				const Position begin = previous().span.begin;
				if (previous().text == "at") {
					scope.kind       = PropertyScope::Kind::AT;
					scope.from_tick  = duration();
					scope.until_tick = scope.from_tick;
					scope.span       = Span{begin, previous().span.end};
					return scope;
				}

				scope.kind = PropertyScope::Kind::DURING;
				require(TokenKind::LEFT_BRACKET, "expected '[' after 'during'");
				scope.from_tick = duration();
				require(TokenKind::COMMA, "expected ',' between scope bounds");
				scope.until_tick   = duration();
				const Token& close = require(TokenKind::RIGHT_BRACKET, "expected ']' after scope bounds");
				scope.span         = Span{begin, close.span.end};
				if (scope.until_tick < scope.from_tick) error(scope.span, "property scope ends before it starts");
				return scope;
			}

			Formula::Interval temporal_interval() {
				Formula::Interval interval;
				const Token&      open = require(TokenKind::LEFT_BRACKET, "expected '[' before temporal bounds");
				interval.from_tick     = duration();
				require(TokenKind::COMMA, "expected ',' between temporal bounds");
				interval.until_tick = duration();
				const Token& close  = require(TokenKind::RIGHT_BRACKET, "expected ']' after temporal bounds");
				interval.span       = Span{open.span.begin, close.span.end};
				if (interval.until_tick < interval.from_tick) {
					error(interval.span, "temporal interval ends before it starts");
				}
				return interval;
			}

			void members(std::unique_ptr<Value>& result) {
				while (take(TokenKind::DOT)) {
					const Token& member = require_name("expected a member name after '.'");
					auto         access = std::make_unique<Value>();
					access->kind        = Value::Kind::MEMBER;
					access->text        = member.text;
					access->span        = Span{result->span.begin, member.span.end};
					access->base        = std::move(result);
					result              = std::move(access);
				}
			}

			std::vector<uint8_t> decode_bytes(const Token& token) const {
				std::vector<uint8_t> result;
				for (std::size_t index = 1; index + 1 < token.text.size(); index++) {
					const unsigned char byte = static_cast<unsigned char>(token.text[index]);
					if (byte != '\\') {
						result.push_back(byte);
						continue;
					}

					const char escape = token.text[++index];
					switch (escape) {
					case '"':  result.push_back('"'); break;
					case '\\': result.push_back('\\'); break;
					case 'n':  result.push_back('\n'); break;
					case 'r':  result.push_back('\r'); break;
					case 't':  result.push_back('\t'); break;
					case 'x':  {
						const std::string digits = token.text.substr(index + 1, 2);
						result.push_back(static_cast<uint8_t>(std::stoul(digits, nullptr, 16)));
						index += 2;
						break;
					}
					default: throw std::logic_error("lexer accepted an invalid byte escape");
					}
				}
				return result;
			}

			ValueType type_name(const Token& token) const {
				if (token.text == "Module") return ValueType::MODULE;
				if (token.text == "Tub") return ValueType::TUB;
				error(token, "unknown domain type '" + token.text + "'");
			}

			void binding_domain(std::string& binding, ValueType& binding_type, std::unique_ptr<Value>& domain,
								const std::string& construct) {
				binding = require_name("expected a bound name after '" + construct + "'").text;
				if (take(TokenKind::COLON)) {
					binding_type = type_name(require_name("expected a type name after ':'"));
					return;
				}
				if (!take_word("in")) error(current(), "expected ':' or 'in' after bound name");
				domain = value();
			}

			std::unique_ptr<Value> count_value(Position begin) {
				auto result  = std::make_unique<Value>();
				result->kind = Value::Kind::COUNT;
				binding_domain(result->binding, result->binding_type, result->domain, "count");
				if (!take_word("where")) error(current(), "expected 'where' after count domain");
				require(TokenKind::LEFT_PAREN, "expected '(' after 'where'");
				result->predicate  = formula();
				const Token& close = require(TokenKind::RIGHT_PAREN, "expected ')' after count predicate");
				result->span       = Span{begin, close.span.end};
				return result;
			}

			std::unique_ptr<Value> value() {
				const Position begin  = current().span.begin;
				auto           result = std::make_unique<Value>();

				if (take_word("count") || take(TokenKind::COUNT_ALIAS)) {
					result = count_value(begin);
				} else if (take(TokenKind::LEFT_PAREN)) {
					result             = value();
					const Token& close = require(TokenKind::RIGHT_PAREN, "expected ')' after value");
					result->span       = Span{begin, close.span.end};
				} else if (take(TokenKind::BYTE_LITERAL)) {
					result->kind  = Value::Kind::BYTES;
					result->text  = previous().text;
					result->bytes = decode_bytes(previous());
					result->span  = previous().span;
				} else if (take(TokenKind::DOT)) {
					const Token& member = require_name("expected an enum member after '.'");
					result->kind        = Value::Kind::ENUM_LITERAL;
					result->text        = member.text;
					result->span        = Span{begin, member.span.end};
				} else {
					bool negative = take(TokenKind::MINUS);
					if (take(TokenKind::NUMBER)) {
						result->kind   = Value::Kind::NUMBER;
						result->text   = (negative ? "-" : "") + previous().text;
						result->number = std::stod(result->text);
						result->span   = Span{begin, previous().span.end};
					} else {
						if (negative) error(current(), "'-' may only prefix a numeric literal");
						if (take(TokenKind::HEX_COLOR)) {
							result->kind  = Value::Kind::COLOR;
							result->text  = previous().text;
							result->color = static_cast<uint32_t>(std::stoul(result->text.substr(2), nullptr, 16));
							result->span  = previous().span;
						} else {
							const Token& name = require_name("expected a value");
							if (name.text == "true" || name.text == "false") {
								result->kind    = Value::Kind::BOOLEAN;
								result->boolean = name.text == "true";
								result->text    = name.text;
							} else {
								result->kind = Value::Kind::NAME;
								result->text = name.text;
							}
							result->span = name.span;

							if (take(TokenKind::LEFT_PAREN)) {
								if (result->kind == Value::Kind::BOOLEAN)
									error(name, "a Boolean literal is not callable");
								result->kind = Value::Kind::CALL;
								if (!at(TokenKind::RIGHT_PAREN)) {
									do {
										result->arguments.push_back(value());
									} while (take(TokenKind::COMMA));
								}
								const Token& close = require(TokenKind::RIGHT_PAREN, "expected ')' after arguments");
								result->span.end   = close.span.end;
							}
						}
					}
				}

				members(result);
				return result;
			}

			[[nodiscard]] bool at_comparison() const noexcept {
				return at(TokenKind::EQUAL_EQUAL) || at(TokenKind::NOT_EQUAL) || at(TokenKind::LESS) ||
					   at(TokenKind::LESS_EQUAL) || at(TokenKind::GREATER) || at(TokenKind::GREATER_EQUAL);
			}

			std::unique_ptr<Formula> comparison(std::unique_ptr<Value> left = nullptr) {
				auto result   = std::make_unique<Formula>();
				result->kind  = Formula::Kind::ATOM;
				result->value = left ? std::move(left) : value();
				result->span  = result->value->span;

				Formula::Comparison operation;
				bool                compared = true;
				if (take(TokenKind::EQUAL_EQUAL)) operation = Formula::Comparison::EQUAL;
				else if (take(TokenKind::NOT_EQUAL)) operation = Formula::Comparison::NOT_EQUAL;
				else if (take(TokenKind::LESS)) operation = Formula::Comparison::LESS;
				else if (take(TokenKind::LESS_EQUAL)) operation = Formula::Comparison::LESS_EQUAL;
				else if (take(TokenKind::GREATER)) operation = Formula::Comparison::GREATER;
				else if (take(TokenKind::GREATER_EQUAL)) operation = Formula::Comparison::GREATER_EQUAL;
				else compared = false;

				if (compared) {
					result->kind       = Formula::Kind::COMPARISON;
					result->comparison = operation;
					result->compared   = value();
					result->span.end   = result->compared->span.end;
				}
				return result;
			}

			struct Operand {
				std::unique_ptr<Formula> formula;
				bool                     grouped  = false;
				bool                     temporal = false;
			};

			Operand quantifier() {
				const Position begin  = current().span.begin;
				auto           result = std::make_unique<Formula>();
				if (take_word("forall") || take(TokenKind::FORALL_ALIAS)) result->kind = Formula::Kind::FORALL;
				else if (take_word("exists") || take(TokenKind::EXISTS_ALIAS)) result->kind = Formula::Kind::EXISTS;
				else throw std::logic_error("quantifier parser called without a quantifier");
				binding_domain(result->binding, result->binding_type, result->domain,
							   result->kind == Formula::Kind::FORALL ? "forall" : "exists");
				require(TokenKind::COLON, "expected ':' before quantifier body");
				result->left = formula();
				result->span = Span{begin, result->left->span.end};
				return Operand{std::move(result), false, true};
			}

			struct SequenceStep {
				std::optional<Formula::Interval> interval;
				std::unique_ptr<Formula>         formula;
			};

			// Expands the steps from `index` onward. Each step is looked for from where the previous
			// one matched, one observation later, so the match ticks strictly increase and a step
			// bound reads as the delay after its predecessor rather than as an absolute offset. A
			// step without a bound is simply unbounded.
			std::unique_ptr<Formula> expand_sequence(std::vector<SequenceStep>& steps, std::size_t index,
													 const Span& span) {
				auto matched = std::move(steps[index].formula);
				if (index + 1 < steps.size()) {
					auto ordering        = std::make_unique<Formula>();
					ordering->kind       = Formula::Kind::NEXT;
					ordering->sequencing = true;
					ordering->span       = span;
					ordering->left       = expand_sequence(steps, index + 1, span);

					auto both   = std::make_unique<Formula>();
					both->kind  = Formula::Kind::AND;
					both->span  = Span{matched->span.begin, ordering->left->span.end};
					both->left  = std::move(matched);
					both->right = std::move(ordering);
					matched     = std::move(both);
				}

				auto search      = std::make_unique<Formula>();
				search->kind     = Formula::Kind::EVENTUALLY;
				search->interval = steps[index].interval;
				search->span     = matched->span;
				search->left     = std::move(matched);
				return search;
			}

			Operand sequence() {
				const Position begin = previous().span.begin;
				require(TokenKind::LEFT_BRACE, "expected '{' after 'sequence'");

				std::vector<SequenceStep> steps;
				while (!at(TokenKind::RIGHT_BRACE)) {
					if (at(TokenKind::END)) error(current(), "expected '}' to close the sequence");
					SequenceStep step;
					// Every step may carry a bound. A later step measures it from the observation
					// after its predecessor matched; the first has no predecessor, so it measures from
					// where the property became active. Leaving the first step unbounded is the usual
					// choice, but it also means the sequence can only ever run out of trace rather
					// than be refuted, because the search for a first match never expires.
					if (at(TokenKind::LEFT_BRACKET)) step.interval = temporal_interval();
					step.formula = formula();
					require(TokenKind::SEMICOLON, "expected ';' after a sequence step");
					steps.push_back(std::move(step));
				}
				const Token& close = require(TokenKind::RIGHT_BRACE, "expected '}' to close the sequence");
				if (steps.empty()) error(Span{begin, close.span.end}, "a sequence needs at least one step");

				const Span span   = Span{begin, close.span.end};
				auto       result = expand_sequence(steps, 0, span);
				result->span      = span;
				return Operand{std::move(result), true, false};
			}

			Operand negation() {
				const Position begin = current().span.begin;
				if (take_word("not")) error(previous(), "word operator 'not' is not supported; use '!'");
				if (take_word("sequence")) return sequence();
				if (take(TokenKind::BANG)) {
					auto result     = std::make_unique<Formula>();
					result->kind    = Formula::Kind::NOT;
					Operand operand = this->operand();
					result->left    = std::move(operand.formula);
					result->span    = Span{begin, result->left->span.end};
					return Operand{std::move(result), false, false};
				}
				if (at_quantifier()) return quantifier();
				if (word("always") || word("G") || word("eventually") || word("F") || word("next") || word("X")) {
					error(current(), "nested temporal operators require explicit parentheses");
				}

				if (take(TokenKind::LEFT_PAREN)) {
					auto         result = formula();
					const Token& close  = require(TokenKind::RIGHT_PAREN, "expected ')' after formula");
					result->span        = Span{begin, close.span.end};
					if (at(TokenKind::DOT) || at_comparison()) {
						if (result->kind != Formula::Kind::ATOM) {
							error(result->span, "only a parenthesized value can have members or be compared");
						}
						auto grouped_value  = std::move(result->value);
						grouped_value->span = Span{begin, close.span.end};
						members(grouped_value);
						return Operand{comparison(std::move(grouped_value)), false, false};
					}
					return Operand{std::move(result), true, false};
				}
				return Operand{comparison(), false, false};
			}

			Operand operand() {
				const Position begin = current().span.begin;
				Formula::Kind  kind;
				bool           temporal = true;
				if (take_word("always") || take_word("G")) kind = Formula::Kind::ALWAYS;
				else if (take_word("eventually") || take_word("F")) kind = Formula::Kind::EVENTUALLY;
				else if (take_word("next") || take_word("X")) kind = Formula::Kind::NEXT;
				else temporal = false;

				if (!temporal) return negation();
				auto result  = std::make_unique<Formula>();
				result->kind = kind;
				if (at(TokenKind::LEFT_BRACKET)) {
					if (kind == Formula::Kind::NEXT) error(current(), "next does not accept a metric interval");
					result->interval = temporal_interval();
				}
				Operand child = negation();
				result->left  = std::move(child.formula);
				result->span  = Span{begin, result->left->span.end};
				return Operand{std::move(result), false, true};
			}

			enum class BinaryFamily : uint8_t {
				NONE,
				AND,
				OR,
				IMPLIES,
				UNTIL,
			};

			BinaryFamily binary_family() {
				if (word("and")) error(current(), "word operator 'and' is not supported; use '&&'");
				if (word("or")) error(current(), "word operator 'or' is not supported; use '||'");
				if (at(TokenKind::AND_AND)) return BinaryFamily::AND;
				if (at(TokenKind::OR_OR)) return BinaryFamily::OR;
				if (at(TokenKind::ARROW)) return BinaryFamily::IMPLIES;
				if (word("until") || word("U")) return BinaryFamily::UNTIL;
				return BinaryFamily::NONE;
			}

			std::optional<Formula::Interval> take_binary(BinaryFamily family) {
				switch (family) {
				case BinaryFamily::AND:     take(TokenKind::AND_AND); break;
				case BinaryFamily::OR:      take(TokenKind::OR_OR); break;
				case BinaryFamily::IMPLIES: take(TokenKind::ARROW); break;
				case BinaryFamily::UNTIL:
					take(TokenKind::IDENTIFIER);
					if (at(TokenKind::LEFT_BRACKET)) return temporal_interval();
					break;
				case BinaryFamily::NONE: break;
				}
				return std::nullopt;
			}

			static Formula::Kind formula_kind(BinaryFamily family) {
				switch (family) {
				case BinaryFamily::AND:     return Formula::Kind::AND;
				case BinaryFamily::OR:      return Formula::Kind::OR;
				case BinaryFamily::IMPLIES: return Formula::Kind::IMPLIES;
				case BinaryFamily::UNTIL:   return Formula::Kind::UNTIL;
				case BinaryFamily::NONE:    break;
				}
				return Formula::Kind::ATOM;
			}

			std::unique_ptr<Formula> combine(BinaryFamily family, std::unique_ptr<Formula> left,
											 std::unique_ptr<Formula>         right,
											 std::optional<Formula::Interval> interval = std::nullopt) {
				auto result      = std::make_unique<Formula>();
				result->kind     = formula_kind(family);
				result->interval = std::move(interval);
				result->left     = std::move(left);
				result->right    = std::move(right);
				result->span     = Span{result->left->span.begin, result->right->span.end};
				return result;
			}

			void require_grouped_temporal(const Operand& value) {
				if (value.temporal && !value.grouped) {
					error(value.formula->span,
						  "temporal operators require parentheses when combined with another operator");
				}
			}

			std::unique_ptr<Formula> formula() {
				Operand      left   = operand();
				BinaryFamily family = binary_family();
				if (family == BinaryFamily::NONE) return std::move(left.formula);

				require_grouped_temporal(left);
				const bool chain = family == BinaryFamily::AND || family == BinaryFamily::OR;
				do {
					std::optional<Formula::Interval> interval = take_binary(family);
					Operand                          right    = operand();
					require_grouped_temporal(right);
					left.formula =
						combine(family, std::move(left.formula), std::move(right.formula), std::move(interval));
					left.grouped = false;
				} while (chain && binary_family() == family);

				if (binary_family() != BinaryFamily::NONE) {
					error(current(), "mixed or non-associative operators require explicit parentheses");
				}
				return std::move(left.formula);
			}

		public:
			Parser(std::vector<Token> tokens, std::string filename)
				: tokens_(std::move(tokens)), filename_(std::move(filename)) {}

			uint64_t duration_only() {
				const uint64_t result = duration();
				require(TokenKind::END, "unexpected text after duration");
				return result;
			}

			File run() {
				File file;
				while (!at(TokenKind::END)) {
					const Position begin = current().span.begin;
					if (take_word("prop")) {
						PropertyDeclaration declaration;
						declaration.name = require_name("expected a proposition name after 'prop'").text;
						if (take_word("at") || take_word("during")) declaration.scope = property_scope();
						require(TokenKind::EQUAL, "expected '=' after proposition name");
						declaration.formula = formula();
						const Token& end    = require(TokenKind::SEMICOLON, "expected ';' after prop declaration");
						declaration.span    = Span{begin, end.span.end};
						file.properties.push_back(std::move(declaration));
					} else {
						error(current(), "expected 'prop' declaration after alias expansion");
					}
				}
				return file;
			}
		};

	} // namespace

	File parse(const std::string& source, const std::string& filename) {
		const std::vector<Token> tokens = lex(source, filename);
		return Parser(MacroExpander(tokens, filename).run(), filename).run();
	}

	uint64_t parse_duration(const std::string& source, const std::string& filename) {
		return Parser(lex(source, filename), filename).duration_only();
	}

} // namespace ltl
