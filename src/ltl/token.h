#ifndef LTL_TOKEN_H
#define LTL_TOKEN_H

#include <string>
#include <vector>

#include "ast.h"

namespace ltl {

	enum class TokenKind : uint8_t {
		END,
		IDENTIFIER,
		NUMBER,
		HEX_COLOR,
		BYTE_LITERAL,
		LEFT_PAREN,
		RIGHT_PAREN,
		LEFT_BRACKET,
		RIGHT_BRACKET,
		LEFT_BRACE,
		RIGHT_BRACE,
		COMMA,
		DOT,
		COLON,
		SEMICOLON,
		EQUAL,
		EQUAL_EQUAL,
		NOT_EQUAL,
		LESS,
		LESS_EQUAL,
		GREATER,
		GREATER_EQUAL,
		ARROW,
		BANG,
		AND_AND,
		OR_OR,
		MINUS,
		FORALL_ALIAS,
		EXISTS_ALIAS,
		COUNT_ALIAS,
	};

	struct Token {
		TokenKind   kind = TokenKind::END;
		std::string text;
		Span        span;
	};

	[[nodiscard]] std::vector<Token> lex(const std::string& source, const std::string& filename = "<ltl>");

} // namespace ltl

#endif // LTL_TOKEN_H
