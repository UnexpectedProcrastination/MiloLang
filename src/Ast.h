#pragma once

#include "Basic.h"
#include "String.h"
#include "Array.h"
#include "BucketedArenaAllocator.h"
#include "Block.h"
#include "TypeTable.h"


enum class CoffJobFlavor : u8 {
	FUNCTION,
	GLOBAL_DECLARATION,
	TYPE
};

struct CoffJob {
	union {
		struct Declaration *declaration;
		struct ExprFunction *function;
		struct Type *type;
	};

	CoffJobFlavor flavor;
};

enum class IrOp : u8 {
	ADD,
	ADD_CONSTANT,
	SUB,
	MUL,
	MUL_BY_CONSTANT,
	DIV,
	DIVIDE_BY_CONSTANT,
	MOD,
	AND,
	OR,
	XOR,
	NOT,
	SHIFT_LEFT,
	SHIFT_RIGHT,
	READ,
	WRITE,
	SET,
	GOTO,
	IF_Z_GOTO,
	IF_NZ_GOTO,
	LESS,
	GREATER,
	LESS_EQUAL,
	GREATER_EQUAL,
	NOT_EQUAL,
	EQUAL,
	ADDRESS_OF_GLOBAL,
	ADDRESS_OF_LOCAL,
	IMMEDIATE,
	FLOAT_TO_INT,
	INT_TO_FLOAT,
	RETURN,
	CALL,
	NEG,
	NOOP,
	FUNCTION, 
	STRING, 
	STRING_EQUAL,
	LINE_MARKER, 
	TYPE, 
	DEFER
};

#define IR_SIGNED_OP 0x1
#define IR_FLOAT_OP 0x2


inline bool isStandardSize(u64 size) {
	return size == 1 || size == 2 || size == 4 || size == 8;
}

#define DEST_NONE UINT64_MAX

struct Argument {
	u64 number;
	struct Type *type;
};

struct FunctionCall {
	struct Type *returnType;
	u64 argCount;
	Argument args[];
};

struct Ir {
	union {
		struct {
			u64 dest;

			union {
				u64 a;
				struct Declaration *declaration;
				struct ExprFunction *function;
				struct ExprStringLiteral *string;
				struct Type *type;
			};

			union {
				u64 b;
				u64 destSize;
				FunctionCall *arguments;
			};

			u8 opSize;
		};

		struct {
			CodeLocation start;
			EndLocation end;
		} location;
	};

	u64 flags = 0;
	IrOp op;
};

struct IrState {
	u64 nextRegister = 1;
	u64 callAuxStorage = 0;
	u64 parameterSpace = 0;
	Array<Ir> ir;

	BucketedArenaAllocator allocator;

	IrState() : allocator(1024) {}
};


enum class TokenT : u8;

enum class ExprFlavor : u8 {
	INT_LITERAL,
	FLOAT_LITERAL, 
	STRING_LITERAL,
	TYPE_LITERAL,
	ARRAY, 
	IDENTIFIER, 

	UNARY_OPERATOR, 
	BINARY_OPERATOR, 
	FUNCTION_CALL, 

	FUNCTION, 

	BLOCK, 
	FOR, 
	WHILE, 
	BREAK, 
	CONTINUE, 
	REMOVE, 
	RETURN, 
	IF, 

	FUNCTION_PROTOTYPE, 
	STRUCT_DEFAULT, 
	ENUM, 
	ENUM_INCREMENT, 

	COMMA_ASSIGNMENT, 

	SWITCH, 

	STATIC_IF, 
	DEFER
};

struct Declaration;

struct Expr {
	CodeLocation start;
	EndLocation end;
	ExprFlavor flavor;

	u64 flags = 0;

	struct Type *type = nullptr;

	Declaration *valueOfDeclaration = nullptr;

#if BUILD_DEBUG // So I don't have to cast it to view the actual expression type in the debugger
	virtual ~Expr() {}
#endif
};

struct ExprDefer : Expr {
	Expr *expr;
	Block *enclosingScope;
};


// The for loop value is being iterated by pointer
#define EXPR_FOR_BY_POINTER 0x1

// This cast was implicitly inserted during type checking
#define EXPR_CAST_IS_IMPLICIT 0x2

// This expression has already been given storage space in the output executable
#define EXPR_HAS_STORAGE 0x4

#define EXPR_FUNCTION_IS_EXTERNAL 0x8


#define EXPR_ARRAY_IS_DYNAMIC 0x10

// Set when identifier resolving passes out of a struct or out of a function, this stops a variable from outside the function being referenced
// since we don't support captures
#define EXPR_IDENTIFIER_RESOLVING_ONLY_CONSTANTS 0x20
#define EXPR_IDENTIFIER_IS_BREAK_OR_CONTINUE_LABEL 0x40

// This is an initializer assignment that was inserted by a variable 
// intitialization in a runtime scope
#define EXPR_ASSIGN_IS_IMPLICIT_INITIALIZER 0x80


// :EvaluatedBinaryLiterals
// Set for consistency in rules that only a literal can be assigned to a constant even though we do 'constexpr' evaluate 
// int + int -> int binary expressions so that expressions such as 3 + 5 can still be converted to unsigned implicitly 
// instead of being stuck as s64. We could allow these to be constants but it would be inconsistent which binary ops are 
// allowed as constants since we don't evaluate comparisons
#define EXPR_WAS_EVALUATED_BINARY 0x100
#define EXPR_VALUE_NOT_REQUIRED 0x200

// These two flags are used to check #must
#define EXPR_FUNCTION_CALL_IS_STATEMENT_LEVEL 0x400     // This is set if the function call is a statement so all return values are ignored
#define EXPR_FUNCTION_CALL_IS_IN_COMMA_ASSIGNMENT 0x800 // This is set if the function call is used as a value in an expression so only the first return value is received

// Set if a comma assignment is a := assignment not just an = assignment, so it should declare its arguments
#define EXPR_COMMA_ASSIGNMENT_IS_DECLARATION 0x1000

// This flag tells us if an integer literal 0 was a -0 that has been evaluated to 0, 
// this is required in the case that the literal is implicitly converted to a float, 
// when we will want the float to be -0.0 instead of just 0
#define EXPR_INTEGER_LITERAL_IS_NEGATIVE_ZERO 0x2000

// When we generate the cases for a switch we create an == ExprBinaryOperator node
// so that we don't have to add an extra case in typechecking, the left side is the 
// value we are switching which is already typechecked so we don't typecheck that again
#define EXPR_EQUALS_IS_IMPLICIT_SWITCH 0x4000

// If a switch if is marked as #complete this flag is set, 
// this means that the switch must include a case for every possible
// value of an enum
#define EXPR_SWITCH_IS_COMPLETE 0x8000

// If a cast is marked as bitwise, the two types must be the same size, 
// there will be no conversion made, the bits are simply reinterpreted
#define EXPR_CAST_IS_BITWISE 0x1'0000

struct ExprLiteral : Expr {
	union {
		u64 unsignedValue;
		s64 signedValue;
		double floatValue;
		struct Type *typeValue;
	};
};

struct ExprSwitch : Expr {
	struct Case {
		Expr *condition;
		Expr *block;
		bool fallsThrough; // @Memory this wastes 7 bytes
		u64 irBranch;
		u64 irSkip;
	};

	Expr *condition;

	Array<Case> cases;
};

struct ExprStringLiteral : Expr {
	String string;

	union Symbol *symbol;
	u32 physicalStorage;
};

struct ExprArray : Expr {
	Expr **storage;
	u64 count;
};

struct ExprBinaryOperator : Expr {
	Expr *left;
	Expr *right;
	TokenT op;
};

struct ExprUnaryOperator : Expr {
	Expr *value;
	TokenT op;
};

struct Block;

struct ExprIdentifier : Expr {
	String name;
	Block *resolveFrom;
	Block *enclosingScope;
	Expr *structAccess;
	u64 indexInBlock;

	Declaration *declaration;
};

// This is an internal compiler expression that is used when the value of an enum is not specified
// it tells the type checker to give it the value next after the previous declaration
struct ExprEnumIncrement : Expr {
	Declaration *previous;
};

struct Arguments {
	u64 count;
	Expr **values;
	String *names;

};

struct ExprFunctionCall : Expr {
	Expr *function;
	Arguments arguments;
};

struct ExprBlock : Expr {
	Block declarations;

	Array<Expr *> exprs;
};

struct ExprFunction : Expr {
	Block arguments;
	Block returns;
	IrState state;

	Expr *body;

	Array<struct SubJob *> sleepingOnMe;

	union Symbol *symbol;
	u32 physicalStorage;
};

struct ExprReturn : Expr {
	ExprFunction *returnsFrom;
	Arguments returns;
};

struct ExprLoop : Expr {
	union {
		Expr *forBegin;
		Expr *whileCondition;
	};
	Expr *forEnd;

	Expr *body;

	Expr *completedBody;

	u64 irPointer;
	u64 arrayPointer;
	Block iteratorBlock;
};

struct ExprBreakOrContinue : Expr {
	Expr *label;
	ExprLoop *refersTo;
};

struct ExprIf : Expr {
	Expr *condition;
	Expr *ifBody;
	Expr *elseBody;
};

struct ExprEnum : Expr {
	TypeEnum struct_;
	Expr *integerType;
};

struct ExprCommaAssignment : Expr {
	u64 exprCount;
	Expr **left;

	Expr *call;
};