#include "parser.h"

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "token.h"

namespace ltl {

	namespace {

		[[noreturn]] void import_error(const std::string& filename, const Token& token, const std::string& message) {
			throw std::invalid_argument(filename + ":" + std::to_string(token.span.begin.line) + ":" +
										std::to_string(token.span.begin.column) + ": " + message);
		}

		std::string import_path(const Token& token, const std::string& filename) {
			std::string result;
			for (std::size_t i = 1; i + 1 < token.text.size(); ++i) {
				if (token.text[i] != '\\') {
					result.push_back(token.text[i]);
					continue;
				}
				const char escape = token.text[++i];
				switch (escape) {
				case '"':  result.push_back('"'); break;
				case '\\': result.push_back('\\'); break;
				default:   import_error(filename, token, "import paths only support the escapes \\\" and \\\\");
				}
			}
			if (result.empty()) import_error(filename, token, "import path cannot be empty");
			return result;
		}

		std::string read_import(const std::filesystem::path& path, const Token& token, const std::string& filename) {
			std::ifstream input(path);
			if (!input) import_error(filename, token, "could not read import '" + path.string() + "'");
			std::ostringstream source;
			source << input.rdbuf();
			return source.str();
		}

		std::vector<Token> imported_tokens(const std::string& source, const std::string& filename,
										   std::unordered_set<std::string>& loaded,
										   std::unordered_set<std::string>& stack) {
			std::vector<Token> input = lex(source, filename);
			std::vector<Token> result;
			std::size_t        i = 0;
			while (i < input.size() && input[i].kind == TokenKind::IDENTIFIER && input[i].text == "import") {
				const Token& keyword = input[i++];
				if (i >= input.size() || input[i].kind != TokenKind::BYTE_LITERAL)
					import_error(filename, input[i], "expected a quoted path after 'import'");
				const Token& path_token = input[i++];
				if (i >= input.size() || input[i].kind != TokenKind::SEMICOLON)
					import_error(filename, input[i], "expected ';' after import");
				++i;

				std::filesystem::path path = import_path(path_token, filename);
				if (path.is_relative()) {
					if (filename.starts_with('<'))
						import_error(filename, path_token, "relative imports require a saved source file");
					path = std::filesystem::path(filename).parent_path() / path;
				}
				path                  = std::filesystem::absolute(path).lexically_normal();
				const std::string key = path.string();
				if (stack.contains(key)) import_error(filename, keyword, "circular import of '" + key + "'");
				if (!loaded.insert(key).second) continue;
				stack.insert(key);
				std::vector<Token> imported =
					imported_tokens(read_import(path, path_token, filename), key, loaded, stack);
				stack.erase(key);
				result.insert(result.end(), imported.begin(), imported.end() - 1);
			}
			for (std::size_t j = i; j < input.size(); ++j) {
				if (input[j].kind == TokenKind::IDENTIFIER && input[j].text == "import")
					import_error(filename, input[j], "imports must be at the top of the file");
			}
			result.insert(result.end(), input.begin() + static_cast<std::ptrdiff_t>(i), input.end());
			return result;
		}

		using Expansion = std::vector<const Token*>;

		template <typename Iterator> Expansion token_refs(Iterator begin, Iterator end) {
			Expansion result;
			result.reserve(end - begin);
			for (; begin != end; ++begin) result.push_back(&*begin);
			return result;
		}

		class MacroExpander {
		private:

			struct Alias {
				std::vector<std::string> parameters;
				Expansion                body;
				std::size_t              order = 0;
			};

			const std::vector<Token>&              tokens_;
			std::string                            filename_;
			std::unordered_map<std::string, Alias> aliases_;
			// Expansion streams borrow tokens. Only hygiene and synthetic parentheses need storage.
			std::deque<Token>                                                       generated_;
			std::unordered_map<const Token*, std::pair<const Token*, const Token*>> parentheses_;

			const Token* parenthesis(const Token& token, bool left) {
				auto [it, inserted] = parentheses_.try_emplace(&token);
				if (inserted) {
					generated_.push_back(Token{TokenKind::LEFT_PAREN, "(", token.span});
					it->second.first = &generated_.back();
					generated_.push_back(Token{TokenKind::RIGHT_PAREN, ")", token.span});
					it->second.second = &generated_.back();
				}
				return left ? it->second.first : it->second.second;
			}

			[[noreturn]] void error(const Token& token, const std::string& message) const {
				const std::string& source =
					token.span.begin.source_name().empty() ? filename_ : token.span.begin.source_name();
				throw std::invalid_argument(source + ":" + std::to_string(token.span.begin.line) + ":" +
											std::to_string(token.span.begin.column) + ": " + message);
			}

			static bool word(const Token& token, const std::string& text) {
				return token.kind == TokenKind::IDENTIFIER && token.text == text;
			}

			static bool snake_case(const std::string& name) {
				if (name.empty() || (name.front() != '_' && (name.front() < 'a' || name.front() > 'z'))) return false;
				for (const char c : name)
					if (c != '_' && (c < 'a' || c > 'z') && (c < '0' || c > '9')) return false;
				return true;
			}

			std::size_t declaration_end(std::size_t begin) const {
				std::size_t braces = 0;
				for (std::size_t i = begin; tokens_[i].kind != TokenKind::END; ++i) {
					if (tokens_[i].kind == TokenKind::LEFT_BRACE) ++braces;
					else if (tokens_[i].kind == TokenKind::RIGHT_BRACE && braces) --braces;
					else if (tokens_[i].kind == TokenKind::SEMICOLON && !braces) return i;
				}
				error(tokens_[begin], word(tokens_[begin], "let") ? "expected ';' after alias declaration"
																  : "expected ';' after prop declaration");
			}

			std::size_t signature_end(std::size_t begin, std::vector<std::string>& parameters) const {
				std::size_t i = begin + 2;
				if (tokens_[i].kind == TokenKind::LEFT_PAREN) {
					++i;
					std::unordered_set<std::string> seen;
					if (tokens_[i].kind != TokenKind::RIGHT_PAREN) {
						for (;;) {
							if (tokens_[i].kind != TokenKind::IDENTIFIER)
								error(tokens_[i], "expected an alias parameter name");
							if (!snake_case(tokens_[i].text))
								error(tokens_[i], "alias parameter names must use snake_case");
							if (!seen.insert(tokens_[i].text).second)
								error(tokens_[i], "duplicate alias parameter '" + tokens_[i].text + "'");
							parameters.push_back(tokens_[i++].text);
							if (tokens_[i].kind != TokenKind::COMMA) break;
							++i;
						}
					}
					if (tokens_[i].kind != TokenKind::RIGHT_PAREN)
						error(tokens_[i], "expected ')' after alias parameters");
					++i;
				}
				if (tokens_[i].kind != TokenKind::EQUAL) error(tokens_[i], "expected '=' after alias declaration");
				return i;
			}

			void discover() {
				std::unordered_set<std::string> names;
				std::size_t                     order = 0;
				for (std::size_t i = 0; tokens_[i].kind != TokenKind::END;) {
					const bool is_let = word(tokens_[i], "let");
					if (!is_let && !word(tokens_[i], "prop")) error(tokens_[i], "expected 'let' or 'prop' declaration");
					if (tokens_[i + 1].kind != TokenKind::IDENTIFIER)
						error(tokens_[i + 1], "expected a declaration name");
					const Token& name = tokens_[i + 1];
					if (!names.insert(name.text).second) error(name, "duplicate declaration '" + name.text + "'");
					if (is_let) {
						if (!snake_case(name.text)) error(name, "alias names must use snake_case");
						std::vector<std::string> params;
						const std::size_t        equal = signature_end(i, params);
						const std::size_t        end   = declaration_end(i);
						if (equal + 1 == end) error(tokens_[end], "expected an alias expression after '='");
						aliases_.emplace(
							name.text, Alias{std::move(params),
											 token_refs(tokens_.begin() + equal + 1, tokens_.begin() + end), order++});
					}
					i = declaration_end(i) + 1;
				}
			}

			static std::size_t matching_paren(const Expansion& input, std::size_t open, const MacroExpander& self) {
				std::size_t depth = 0;
				for (std::size_t i = open; i < input.size(); ++i) {
					if (input[i]->kind == TokenKind::LEFT_PAREN) ++depth;
					else if (input[i]->kind == TokenKind::RIGHT_PAREN && --depth == 0) return i;
				}
				self.error(*input[open], "expected ')' after alias arguments");
			}

			Expansion hygienic_body(const Alias& alias) {
				Expansion                                    body = alias.body;
				std::unordered_map<std::string, std::string> renamed;
				for (std::size_t i = 0; i + 1 < body.size(); ++i) {
					const bool binder = word(*body[i], "forall") || word(*body[i], "exists") ||
										word(*body[i], "count") || body[i]->kind == TokenKind::FORALL_ALIAS ||
										body[i]->kind == TokenKind::EXISTS_ALIAS ||
										body[i]->kind == TokenKind::COUNT_ALIAS;
					if (binder && body[i + 1]->kind == TokenKind::IDENTIFIER) {
						renamed.emplace(body[i + 1]->text,
										"__alias_" + std::to_string(alias.order) + "_" + body[i + 1]->text);
					}
				}
				for (std::size_t i = 0; i < body.size(); ++i) {
					if (body[i]->kind != TokenKind::IDENTIFIER || (i && body[i - 1]->kind == TokenKind::DOT)) continue;
					if (const auto found = renamed.find(body[i]->text); found != renamed.end()) {
						generated_.push_back(*body[i]);
						generated_.back().text = found->second;
						body[i]                = &generated_.back();
					}
				}
				return body;
			}

			Expansion expand_range(const Expansion& input, std::size_t begin, std::size_t end, std::size_t available,
								   const std::unordered_map<std::string, Expansion>& substitutions,
								   std::unordered_set<std::string>&                  stack) {
				Expansion out;
				for (std::size_t i = begin; i < end; ++i) {
					const Token& token  = *input[i];
					const bool   member = i > begin && input[i - 1]->kind == TokenKind::DOT;
					if (token.kind != TokenKind::IDENTIFIER || member) {
						out.push_back(&token);
						continue;
					}
					const bool bound_name =
						i > begin &&
						(word(*input[i - 1], "forall") || word(*input[i - 1], "exists") ||
						 word(*input[i - 1], "count") || input[i - 1]->kind == TokenKind::FORALL_ALIAS ||
						 input[i - 1]->kind == TokenKind::EXISTS_ALIAS || input[i - 1]->kind == TokenKind::COUNT_ALIAS);
					if (bound_name && aliases_.contains(token.text))
						error(token, "bound name '" + token.text + "' shadows an alias");
					if (const auto sub = substitutions.find(token.text); sub != substitutions.end()) {
						out.push_back(parenthesis(token, true));
						out.insert(out.end(), sub->second.begin(), sub->second.end());
						out.push_back(parenthesis(token, false));
						continue;
					}
					const auto found = aliases_.find(token.text);
					if (found == aliases_.end()) {
						out.push_back(&token);
						continue;
					}
					const Alias& alias = found->second;
					if (alias.order >= available)
						error(token, "alias '" + token.text + "' cannot be referenced before its declaration");
					if (!stack.insert(token.text).second)
						error(token, "recursive alias '" + token.text + "' is not allowed");
					std::unordered_map<std::string, Expansion> args;
					if (!alias.parameters.empty()) {
						if (i + 1 >= end || input[i + 1]->kind != TokenKind::LEFT_PAREN)
							error(token, "alias '" + token.text + "' expects " +
											 std::to_string(alias.parameters.size()) + " arguments");
						const std::size_t      close = matching_paren(input, i + 1, *this);
						std::vector<Expansion> actual;
						std::size_t            start = i + 2, depth = 0;
						for (std::size_t j = start; j <= close; ++j) {
							const bool separator = j == close || (input[j]->kind == TokenKind::COMMA && depth == 0);
							if (separator) {
								if (j > start)
									actual.push_back(expand_range(input, start, j, available, substitutions, stack));
								else if (j != close || start != close) actual.emplace_back();
								start = j + 1;
							} else if (input[j]->kind == TokenKind::LEFT_PAREN ||
									   input[j]->kind == TokenKind::LEFT_BRACKET)
								++depth;
							else if ((input[j]->kind == TokenKind::RIGHT_PAREN ||
									  input[j]->kind == TokenKind::RIGHT_BRACKET) &&
									 depth)
								--depth;
						}
						if (actual.size() != alias.parameters.size())
							error(token, "alias '" + token.text + "' expects " +
											 std::to_string(alias.parameters.size()) + " arguments, got " +
											 std::to_string(actual.size()));
						for (std::size_t n = 0; n < actual.size(); ++n)
							args.emplace(alias.parameters[n], std::move(actual[n]));
						i = close;
					} else if (i + 1 < end && input[i + 1]->kind == TokenKind::LEFT_PAREN) {
						error(token, "alias '" + token.text + "' expects 0 arguments");
					}
					Expansion expanded = expand_range(alias.body, 0, alias.body.size(), alias.order, args, stack);
					stack.erase(token.text);
					out.push_back(parenthesis(token, true));
					out.insert(out.end(), expanded.begin(), expanded.end());
					out.push_back(parenthesis(token, false));
				}
				return out;
			}

			void reject_free_names(const std::string& name, const Alias& alias) const {
				std::unordered_set<std::string>              allowed(alias.parameters.begin(), alias.parameters.end());
				static const std::unordered_set<std::string> words = {"true",
																	  "false",
																	  "always",
																	  "eventually",
																	  "next",
																	  "until",
																	  "G",
																	  "F",
																	  "X",
																	  "U",
																	  "forall",
																	  "exists",
																	  "count",
																	  "in",
																	  "where",
																	  "sequence",
																	  "Module",
																	  "Tub",
																	  "h",
																	  "m",
																	  "s",
																	  "ms",
																	  "module",
																	  "tub",
																	  "rpc_response_to",
																	  "rpc_requests",
																	  "rpc_responses",
																	  "rpc_events",
																	  "present",
																	  "seen",
																	  "at_module",
																	  "at_position",
																	  "appeared",
																	  "disappeared",
																	  "physical_exit",
																	  "exit_cause",
																	  "exit_endpoint",
																	  "exit_module",
																	  "exit_time",
																	  "exit_data",
																	  "exit_uid",
																	  "initial_data",
																	  "initial_uid",
																	  "entered",
																	  "left",
																	  "changed"};
				for (std::size_t i = 0; i < alias.body.size(); ++i) {
					const Token& t = *alias.body[i];
					if (t.kind != TokenKind::IDENTIFIER || (i && alias.body[i - 1]->kind == TokenKind::DOT)) continue;
					if ((t.text == "forall" || t.text == "exists" || t.text == "count") && i + 1 < alias.body.size()) {
						allowed.insert(alias.body[i + 1]->text);
						continue;
					}
					const auto other = aliases_.find(t.text);
					if (allowed.contains(t.text) || words.contains(t.text) ||
						(other != aliases_.end() && other->second.order < alias.order))
						continue;
					if (other != aliases_.end() && other->second.order == alias.order)
						error(t, "recursive alias '" + name + "' is not allowed");
					if (other != aliases_.end())
						error(t, "alias '" + t.text + "' cannot be referenced before its declaration");
					error(t, "alias '" + name + "' has unresolved free name '" + t.text +
								 "'; add it as an explicit parameter");
				}
			}

		public:
			MacroExpander(const std::vector<Token>& tokens, std::string filename)
				: tokens_(tokens), filename_(std::move(filename)) {}

			Expansion run() {
				discover();
				for (const auto& [name, alias] : aliases_) reject_free_names(name, alias);
				for (auto& [name, alias] : aliases_) alias.body = hygienic_body(alias);
				Expansion   result;
				std::size_t available = 0;
				for (std::size_t i = 0; tokens_[i].kind != TokenKind::END;) {
					const std::size_t end = declaration_end(i);
					if (word(tokens_[i], "let")) {
						++available;
						i = end + 1;
						continue;
					}
					std::size_t equal = i + 2;
					while (equal < end && tokens_[equal].kind != TokenKind::EQUAL) ++equal;
					if (equal == end) error(tokens_[end], "expected '=' after proposition name");
					for (std::size_t j = i; j <= equal; ++j) result.push_back(&tokens_[j]);
					Expansion body = token_refs(tokens_.begin() + equal + 1, tokens_.begin() + end);
					std::unordered_set<std::string> stack;
					Expansion                       expanded = expand_range(body, 0, body.size(), available, {}, stack);
					result.insert(result.end(), std::make_move_iterator(expanded.begin()),
								  std::make_move_iterator(expanded.end()));
					result.push_back(&tokens_[end]);
					i = end + 1;
				}
				result.push_back(&tokens_.back());
				return result;
			}
		};

		class Parser {
		private:
			Expansion   tokens_;
			std::string filename_;
			std::size_t current_ = 0;

			[[nodiscard]] const Token& current() const noexcept { return *tokens_[current_]; }

			[[nodiscard]] const Token& previous() const noexcept { return *tokens_[current_ - 1]; }

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
				const std::string& source =
					token.span.begin.source_name().empty() ? filename_ : token.span.begin.source_name();
				throw std::invalid_argument(source + ":" + std::to_string(token.span.begin.line) + ":" +
											std::to_string(token.span.begin.column) + ": " + message);
			}

			[[noreturn]] void error(const Span& span, const std::string& message) const {
				const std::string& source = span.begin.source_name().empty() ? filename_ : span.begin.source_name();
				throw std::invalid_argument(source + ":" + std::to_string(span.begin.line) + ":" +
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

			void postfix(std::unique_ptr<Value>& result) {
				for (;;) {
					if (take(TokenKind::DOT)) {
						const Token& member = require_name("expected a member name after '.'");
						auto         access = std::make_unique<Value>();
						access->kind        = Value::Kind::MEMBER;
						access->text        = member.text;
						access->span        = Span{result->span.begin, member.span.end};
						access->base        = std::move(result);
						result              = std::move(access);
						continue;
					}
					if (take(TokenKind::LEFT_BRACKET)) {
						auto access  = std::make_unique<Value>();
						access->kind = Value::Kind::INDEX;
						access->base = std::move(result);
						access->arguments.push_back(value());
						if (take(TokenKind::COLON)) {
							access->kind = Value::Kind::SLICE;
							access->arguments.push_back(value());
						}
						const Token& close = require(TokenKind::RIGHT_BRACKET, access->kind == Value::Kind::SLICE
																				   ? "expected ']' after byte slice"
																				   : "expected ']' after byte index");
						access->span       = Span{access->base->span.begin, close.span.end};
						result             = std::move(access);
						continue;
					}
					break;
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
					case 'x':
						result.push_back(
							static_cast<uint8_t>(std::stoul(token.text.substr(index + 1, 2), nullptr, 16)));
						index += 2;
						break;
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

			std::unique_ptr<Value> primary() {
				const Position begin  = current().span.begin;
				auto           result = std::make_unique<Value>();
				if (take_word("count") || take(TokenKind::COUNT_ALIAS)) result = count_value(begin);
				else if (take(TokenKind::LEFT_PAREN)) {
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
				} else if (take(TokenKind::NUMBER)) {
					result->kind = Value::Kind::NUMBER;
					result->text = previous().text;
					result->span = previous().span;
					if (result->text.starts_with("0x") || result->text.starts_with("0X")) {
						std::string    digits = result->text.substr(2, result->text.size() - 3);
						const uint64_t raw    = std::stoull(digits, nullptr, 16);
						result->integer       = static_cast<int64_t>(raw);
						result->number        = static_cast<double>(result->integer);
						result->is_integer    = true;
					} else {
						result->number     = std::stod(result->text);
						result->is_integer = result->text.find('.') == std::string::npos;
						if (result->is_integer) result->integer = std::stoll(result->text);
					}
				} else if (take(TokenKind::HEX_COLOR)) {
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
						if (result->kind == Value::Kind::BOOLEAN) error(name, "a Boolean literal is not callable");
						result->kind = Value::Kind::CALL;
						if (!at(TokenKind::RIGHT_PAREN)) do {
								result->arguments.push_back(value());
							} while (take(TokenKind::COMMA));
						const Token& close = require(TokenKind::RIGHT_PAREN, "expected ')' after arguments");
						result->span.end   = close.span.end;
					}
				}
				postfix(result);
				return result;
			}

			std::unique_ptr<Value> unary_value() {
				if (at(TokenKind::MINUS) || at(TokenKind::TILDE)) {
					const Token op = current();
					++current_;
					auto operand = unary_value();
					if (op.kind == TokenKind::MINUS && operand->kind == Value::Kind::NUMBER) {
						operand->span.begin = op.span.begin;
						operand->text       = "-" + operand->text;
						operand->number     = -operand->number;
						if (operand->is_integer)
							operand->integer = static_cast<int64_t>(0U - static_cast<uint64_t>(operand->integer));
						return operand;
					}
					auto result  = std::make_unique<Value>();
					result->kind = Value::Kind::UNARY;
					result->operation =
						op.kind == TokenKind::MINUS ? Value::Operator::NEGATE : Value::Operator::BIT_NOT;
					result->base = std::move(operand);
					result->span = Span{op.span.begin, result->base->span.end};
					return result;
				}
				return primary();
			}

			std::unique_ptr<Value> binary_value(std::unique_ptr<Value> left, Value::Operator op,
												std::unique_ptr<Value> right) {
				auto result       = std::make_unique<Value>();
				result->kind      = Value::Kind::BINARY;
				result->operation = op;
				result->span      = Span{left->span.begin, right->span.end};
				result->base      = std::move(left);
				result->arguments.push_back(std::move(right));
				return result;
			}

			std::unique_ptr<Value> multiplicative() {
				auto left = unary_value();
				for (;;) {
					Value::Operator op;
					if (take(TokenKind::STAR)) op = Value::Operator::MULTIPLY;
					else if (take(TokenKind::SLASH)) op = Value::Operator::DIVIDE;
					else if (take(TokenKind::PERCENT)) op = Value::Operator::MODULO;
					else break;
					left = binary_value(std::move(left), op, unary_value());
				}
				return left;
			}

			std::unique_ptr<Value> additive() {
				auto left = multiplicative();
				for (;;) {
					Value::Operator op;
					if (take(TokenKind::PLUS)) op = Value::Operator::ADD;
					else if (take(TokenKind::MINUS)) op = Value::Operator::SUBTRACT;
					else break;
					left = binary_value(std::move(left), op, multiplicative());
				}
				return left;
			}

			std::unique_ptr<Value> shifts() {
				auto left = additive();
				for (;;) {
					Value::Operator op;
					if (take(TokenKind::SHIFT_LEFT)) op = Value::Operator::SHIFT_LEFT;
					else if (take(TokenKind::SHIFT_RIGHT)) op = Value::Operator::SHIFT_RIGHT;
					else break;
					left = binary_value(std::move(left), op, additive());
				}
				return left;
			}

			std::unique_ptr<Value> bit_and() {
				auto left = shifts();
				while (take(TokenKind::AMPERSAND))
					left = binary_value(std::move(left), Value::Operator::BIT_AND, shifts());
				return left;
			}

			std::unique_ptr<Value> bit_xor() {
				auto left = bit_and();
				while (take(TokenKind::CARET))
					left = binary_value(std::move(left), Value::Operator::BIT_XOR, bit_and());
				return left;
			}

			int value_precedence(TokenKind kind) const {
				switch (kind) {
				case TokenKind::PIPE:        return 1;
				case TokenKind::CARET:       return 2;
				case TokenKind::AMPERSAND:   return 3;
				case TokenKind::SHIFT_LEFT:
				case TokenKind::SHIFT_RIGHT: return 4;
				case TokenKind::PLUS:
				case TokenKind::MINUS:       return 5;
				case TokenKind::STAR:
				case TokenKind::SLASH:
				case TokenKind::PERCENT:     return 6;
				default:                     return 0;
				}
			}

			Value::Operator value_operation(TokenKind kind) const {
				switch (kind) {
				case TokenKind::PIPE:        return Value::Operator::BIT_OR;
				case TokenKind::CARET:       return Value::Operator::BIT_XOR;
				case TokenKind::AMPERSAND:   return Value::Operator::BIT_AND;
				case TokenKind::SHIFT_LEFT:  return Value::Operator::SHIFT_LEFT;
				case TokenKind::SHIFT_RIGHT: return Value::Operator::SHIFT_RIGHT;
				case TokenKind::PLUS:        return Value::Operator::ADD;
				case TokenKind::MINUS:       return Value::Operator::SUBTRACT;
				case TokenKind::STAR:        return Value::Operator::MULTIPLY;
				case TokenKind::SLASH:       return Value::Operator::DIVIDE;
				case TokenKind::PERCENT:     return Value::Operator::MODULO;
				default:                     return Value::Operator::NONE;
				}
			}

			std::unique_ptr<Value> value_tail(std::unique_ptr<Value> left, int minimum) {
				while (value_precedence(current().kind) >= minimum) {
					const TokenKind token      = current().kind;
					const int       precedence = value_precedence(token);
					++current_;
					auto right = value_tail(unary_value(), precedence + 1);
					left       = binary_value(std::move(left), value_operation(token), std::move(right));
				}
				return left;
			}

			std::unique_ptr<Value> value() { return value_tail(unary_value(), 1); }

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
					if (at(TokenKind::DOT) || at(TokenKind::LEFT_BRACKET) || value_precedence(current().kind) > 0 ||
						at_comparison()) {
						if (result->kind != Formula::Kind::ATOM) {
							error(result->span, "only a parenthesized value can have members or be compared");
						}
						auto grouped_value  = std::move(result->value);
						grouped_value->span = Span{begin, close.span.end};
						postfix(grouped_value);
						grouped_value = value_tail(std::move(grouped_value), 1);
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
			Parser(Expansion tokens, std::string filename)
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
		std::unordered_set<std::string> loaded;
		std::unordered_set<std::string> stack;
		if (!filename.starts_with('<')) stack.insert(std::filesystem::absolute(filename).lexically_normal().string());
		const std::vector<Token> tokens = imported_tokens(source, filename, loaded, stack);
		MacroExpander            expander(tokens, filename);
		return Parser(expander.run(), filename).run();
	}

	uint64_t parse_duration(const std::string& source, const std::string& filename) {
		const auto tokens = lex(source, filename);
		return Parser(token_refs(tokens.begin(), tokens.end()), filename).duration_only();
	}

} // namespace ltl
