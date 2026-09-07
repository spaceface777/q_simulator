#ifndef LTL_AST_H
#define LTL_AST_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ltl {
	struct Formula;

	struct Position {
		std::size_t offset = 0;
		std::size_t line   = 1;
		std::size_t column = 1;
	};

	struct Span {
		Position begin;
		Position end;
	};

	enum class ValueType : uint8_t {
		UNKNOWN,
		BOOLEAN,
		NUMBER,
		COLOR,
		MODULE,
		TUB,
		POSITION,
		BYTES,
		RPC_COMMAND,
		RPC_STATUS,
		RPC_EVENT_KIND,
		RPC_REQUEST,
		RPC_RESPONSE,
		RPC_EVENT,
	};

	struct Value {
		enum class Kind : uint8_t {
			BOOLEAN,
			NUMBER,
			COLOR,
			BYTES,
			ENUM_LITERAL,
			COUNT,
			NAME,
			CALL,
			MEMBER,
			INDEX,
			SLICE,
			UNARY,
			BINARY,
		};

		enum class Operator : uint8_t {
			NONE,
			NEGATE,
			BIT_NOT,
			ADD,
			SUBTRACT,
			MULTIPLY,
			DIVIDE,
			MODULO,
			SHIFT_LEFT,
			SHIFT_RIGHT,
			BIT_AND,
			BIT_XOR,
			BIT_OR,
		};

		enum class Symbol : uint8_t {
			NONE,
			PRESENT,
			AT_MODULE,
			AT_POSITION,
			APPEARED,
			DISAPPEARED,
			PHYSICAL_EXIT,
			EXIT_CAUSE,
			EXIT_ENDPOINT,
			EXIT_MODULE,
			EXIT_TIME,
			EXIT_DATA,
			EXIT_UID,
			INITIAL_DATA,
			INITIAL_UID,
			ENTERED,
			LEFT,
			CHANGED,
			ID,
			CALL_ID,
			MODULE,
			CMD,
			PAYLOAD,
			DATA,
			UID,
			LENGTH,
			CONFIRMABLE,
			REQUEST,
			STATUS,
			KIND,
			BIG_BELT_SPEED,
			SMALL_BELT_SPEED,
			SERVO_ANGLE,
			SERVO_PHYSICAL_ANGLE,
			LED_COLOR,
			LEFT_LASER_ENABLED,
			RIGHT_LASER_ENABLED,
			LEFT_LASER_BLOCKED,
			RIGHT_LASER_BLOCKED,
			RFID_ACTIVE,
			ENTRY,
			CENTER,
			LEFT_LASER,
			RIGHT_LASER,
		};

		Kind                                kind = Kind::NAME;
		Span                                span;
		ValueType                           type = ValueType::UNKNOWN;
		std::string                         text;
		double                              number     = 0.0;
		int64_t                             integer    = 0;
		bool                                is_integer = false;
		uint32_t                            color      = 0;
		std::vector<uint8_t>                bytes;
		uint64_t                            entity_id  = 0;
		int64_t                             enum_value = 0;
		Symbol                              symbol     = Symbol::NONE;
		Operator                            operation  = Operator::NONE;
		bool                                boolean    = false;
		std::unique_ptr<Value>              base;
		std::vector<std::unique_ptr<Value>> arguments;
		std::string                         binding;
		ValueType                           binding_type = ValueType::UNKNOWN;
		std::unique_ptr<Value>              domain;
		std::unique_ptr<Formula>            predicate;
	};

	struct Formula {
		enum class Kind : uint8_t {
			ATOM,
			COMPARISON,
			NOT,
			ALWAYS,
			EVENTUALLY,
			NEXT,
			AND,
			OR,
			IMPLIES,
			UNTIL,
			FORALL,
			EXISTS,
		};

		enum class Comparison : uint8_t {
			EQUAL,
			NOT_EQUAL,
			LESS,
			LESS_EQUAL,
			GREATER,
			GREATER_EQUAL,
		};

		Kind kind = Kind::ATOM;
		Span span;

		struct Interval {
			Span     span;
			uint64_t from_tick  = 0;
			uint64_t until_tick = 0;
		};

		std::optional<Interval> interval;

		// NEXT advances on the next observation in which its subject could have advanced. A module's
		// code runs only on the ticks its own clock grants, so the checker resolves which module a
		// `next` speaks about. Zero means the global observation tick, which is the right answer for
		// a formula mentioning no module: the physical world advances on every tick. The binding name
		// is set instead when the module comes from an enclosing quantifier, and is resolved to an id
		// when that quantifier grounds the formula.
		uint16_t    step_module = 0;
		std::string step_module_binding;

		// Set on the NEXT a `sequence` expands to, which orders steps against the observation tick
		// every module shares rather than against any one module's clock. Steps are bounded in wall
		// time, so the module that happens to appear in a step must not shift when the next begins.
		bool sequencing = false;

		Comparison               comparison = Comparison::EQUAL;
		std::unique_ptr<Value>   value;
		std::unique_ptr<Value>   compared;
		std::unique_ptr<Formula> left;
		std::unique_ptr<Formula> right;
		std::string              binding;
		ValueType                binding_type = ValueType::UNKNOWN;
		std::unique_ptr<Value>   domain;
	};

	struct PropertyScope {
		enum class Kind : uint8_t {
			AT,
			DURING,
		};

		Kind     kind = Kind::AT;
		Span     span;
		uint64_t from_tick  = 0;
		uint64_t until_tick = 0;
	};

	struct PropertyDeclaration {
		std::string                  name;
		Span                         span;
		std::optional<PropertyScope> scope;
		std::unique_ptr<Formula>     formula;
	};

	struct File {
		std::vector<PropertyDeclaration> properties;
	};

} // namespace ltl

#endif // LTL_AST_H
