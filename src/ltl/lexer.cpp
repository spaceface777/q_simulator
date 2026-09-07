#include "token.h"

#include <cctype>
#include <stdexcept>

namespace ltl {

	namespace {

		class Lexer {
		private:
			const std::string& source_;
			std::string        filename_;
			Position           position_;
			std::vector<Token> tokens_;

			[[nodiscard]] bool done() const noexcept { return position_.offset >= source_.size(); }

			[[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
				const std::size_t offset = position_.offset + ahead;
				return offset < source_.size() ? source_[offset] : '\0';
			}

			char take() {
				const char character = source_[position_.offset++];
				if (character == '\n') {
					position_.line++;
					position_.column = 1;
				} else {
					position_.column++;
				}
				return character;
			}

			void add(TokenKind kind, Position begin) {
				tokens_.push_back(
					Token{kind, source_.substr(begin.offset, position_.offset - begin.offset), Span{begin, position_}});
			}

			[[noreturn]] void error(const std::string& message) const {
				throw std::invalid_argument(filename_ + ":" + std::to_string(position_.line) + ":" +
											std::to_string(position_.column) + ": " + message);
			}

			void identifier() {
				const Position begin = position_;
				if (!tokens_.empty() && tokens_.back().kind == TokenKind::NUMBER) {
					const bool single_unit_before_number = (peek() == 'h' || peek() == 'm' || peek() == 's') &&
														   std::isdigit(static_cast<unsigned char>(peek(1)));
					const bool milliseconds_before_number =
						peek() == 'm' && peek(1) == 's' && std::isdigit(static_cast<unsigned char>(peek(2)));
					if (milliseconds_before_number) {
						take();
						take();
						add(TokenKind::IDENTIFIER, begin);
						return;
					}
					if (single_unit_before_number) {
						take();
						add(TokenKind::IDENTIFIER, begin);
						return;
					}
				}
				do {
					take();
				} while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_');
				add(TokenKind::IDENTIFIER, begin);
			}

			void number() {
				const Position begin = position_;
				if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
					take();
					take();
					std::size_t digits = 0;
					while (std::isxdigit(static_cast<unsigned char>(peek()))) {
						take();
						digits++;
					}
					if (peek() == 'u' || peek() == 'U') {
						if (digits == 0 || digits > 16) error("hexadecimal integer must contain 1 through 16 digits");
						take();
						add(TokenKind::NUMBER, begin);
						return;
					}
					if (digits != 6)
						error("RGB colours must contain exactly six hexadecimal digits; add 'u' for an integer");
					add(TokenKind::HEX_COLOR, begin);
					return;
				}

				while (std::isdigit(static_cast<unsigned char>(peek()))) take();
				if (peek() == '.') {
					take();
					if (!std::isdigit(static_cast<unsigned char>(peek())))
						error("expected a digit after decimal point");
					while (std::isdigit(static_cast<unsigned char>(peek()))) take();
				}
				add(TokenKind::NUMBER, begin);
			}

			void byte_literal() {
				const Position begin = position_;
				take();
				while (!done() && peek() != '"') {
					if (peek() == '\n' || peek() == '\r') error("newline in byte literal");
					if (peek() != '\\') {
						take();
						continue;
					}

					take();
					if (done()) error("unterminated byte literal");
					const char escape = take();
					if (escape == '"' || escape == '\\' || escape == 'n' || escape == 'r' || escape == 't') {
						continue;
					}
					if (escape == 'x') {
						if (!std::isxdigit(static_cast<unsigned char>(peek())) ||
							!std::isxdigit(static_cast<unsigned char>(peek(1)))) {
							error("'\\x' escape requires exactly two hexadecimal digits");
						}
						take();
						take();
						continue;
					}
					error("unknown byte escape '\\" + std::string(1, escape) + "'");
				}
				if (done()) error("unterminated byte literal");
				take();
				add(TokenKind::BYTE_LITERAL, begin);
			}

			bool pair(char second, TokenKind kind, Position begin) {
				if (peek() != second) return false;
				take();
				add(kind, begin);
				return true;
			}

		public:
			Lexer(const std::string& source, std::string filename) : source_(source), filename_(std::move(filename)) {}

			std::vector<Token> run() {
				while (!done()) {
					if (std::isspace(static_cast<unsigned char>(peek()))) {
						take();
						continue;
					}
					if (peek() == '/' && peek(1) == '/') {
						while (!done() && peek() != '\n') take();
						continue;
					}
					if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_') {
						identifier();
						continue;
					}
					if (std::isdigit(static_cast<unsigned char>(peek()))) {
						number();
						continue;
					}
					if (peek() == '"') {
						byte_literal();
						continue;
					}

					const Position begin     = position_;
					const char     character = take();
					switch (character) {
					case '(': add(TokenKind::LEFT_PAREN, begin); break;
					case ')': add(TokenKind::RIGHT_PAREN, begin); break;
					case '[': add(TokenKind::LEFT_BRACKET, begin); break;
					case '{': add(TokenKind::LEFT_BRACE, begin); break;
					case '}': add(TokenKind::RIGHT_BRACE, begin); break;
					case ']': add(TokenKind::RIGHT_BRACKET, begin); break;
					case ',': add(TokenKind::COMMA, begin); break;
					case '.': add(TokenKind::DOT, begin); break;
					case ':': add(TokenKind::COLON, begin); break;
					case ';': add(TokenKind::SEMICOLON, begin); break;
					case '=':
						if (!pair('=', TokenKind::EQUAL_EQUAL, begin)) add(TokenKind::EQUAL, begin);
						break;
					case '!':
						if (!pair('=', TokenKind::NOT_EQUAL, begin)) add(TokenKind::BANG, begin);
						break;
					case '<':
						if (pair('=', TokenKind::LESS_EQUAL, begin)) break;
						if (pair('<', TokenKind::SHIFT_LEFT, begin)) break;
						add(TokenKind::LESS, begin);
						break;
					case '>':
						if (pair('=', TokenKind::GREATER_EQUAL, begin)) break;
						if (pair('>', TokenKind::SHIFT_RIGHT, begin)) break;
						add(TokenKind::GREATER, begin);
						break;
					case '+': add(TokenKind::PLUS, begin); break;
					case '-':
						if (!pair('>', TokenKind::ARROW, begin)) add(TokenKind::MINUS, begin);
						break;
					case '*': add(TokenKind::STAR, begin); break;
					case '/': add(TokenKind::SLASH, begin); break;
					case '%': add(TokenKind::PERCENT, begin); break;
					case '^': add(TokenKind::CARET, begin); break;
					case '~': add(TokenKind::TILDE, begin); break;
					case '&':
						if (!pair('&', TokenKind::AND_AND, begin)) add(TokenKind::AMPERSAND, begin);
						break;
					case '|':
						if (!pair('|', TokenKind::OR_OR, begin)) add(TokenKind::PIPE, begin);
						break;
					case '\\':
						if (peek() == 'A') {
							take();
							add(TokenKind::FORALL_ALIAS, begin);
						} else if (peek() == 'E') {
							take();
							add(TokenKind::EXISTS_ALIAS, begin);
						} else {
							error("expected 'A' or 'E' after '\\'");
						}
						break;
					case '#': add(TokenKind::COUNT_ALIAS, begin); break;
					default:  error("unexpected character '" + std::string(1, character) + "'");
					}
				}
				tokens_.push_back(Token{TokenKind::END, "", Span{position_, position_}});
				return std::move(tokens_);
			}
		};

	} // namespace

	std::vector<Token> lex(const std::string& source, const std::string& filename) {
		return Lexer(source, filename).run();
	}

} // namespace ltl
