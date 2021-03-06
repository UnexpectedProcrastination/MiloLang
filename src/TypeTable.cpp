#pragma once

#include "Basic.h"
#include "TypeTable.h"
#include "Ast.h"
#include "CoffWriter.h"

BucketedArenaAllocator typeArena(1024 * 1024);

#define STRUCT_HASH_PRIME 4057
#define POINTER_HASH_PRIME 4241
#define ARRAY_HASH_PRIME 4423
#define DYNAMIC_ARRAY_HASH_PRIME 4603
#define STATIC_ARRAY_SIZE_HASH_PRIME 4789
#define STATIC_ARRAY_TYPE_HASH_PRIME 4969
#define FUNCTION_ARGUMENT_HASH_PRIME 5147
#define FUNCTION_RETURN_HASH_PRIME 5347
#define C_CALL_HASH_PRIME 6073
#define VARARGS_HASH_PRIME 6221
#define COMPILER_HASH_PRIME 6247

static u32 nextStructHash = STRUCT_HASH_PRIME;

static u32 count;

void rehash() {
	PROFILE_FUNC();
	u32 newCapacity = typeTableCapacity * 2;
	TypeTableEntry *newEntries = new TypeTableEntry[newCapacity];

	for (u32 i = 0; i < typeTableCapacity; i++) {
		TypeTableEntry entry = typeTableEntries[i];

		if (entry.hash == 0) continue;

		u32 slot = entry.hash & (newCapacity - 1);

		while (newEntries[slot].hash) {
			if (++slot == newCapacity) slot = 0;
		}

		newEntries[slot].hash =entry.hash;
		newEntries[slot].value = entry.value;
	}

	delete[] typeTableEntries;
	typeTableEntries = newEntries;
	typeTableCapacity = newCapacity;
}

void insertIntoTable(Type *type, u32 slot) {
	PROFILE_FUNC();
	if (count * 10 > typeTableCapacity * 7) {
		rehash();

		slot = type->hash & (typeTableCapacity - 1);

		while (typeTableEntries[slot].hash) {
			if (++slot == typeTableCapacity) slot = 0;
		}
	}

	typeTableEntries[slot].value = type;
	typeTableEntries[slot].hash = type->hash;
}

void insertIntoTable(Type *type) {
	PROFILE_FUNC();
	if (count * 10 > typeTableCapacity * 7) {
		rehash();
	}

	u32 slot = type->hash & (typeTableCapacity - 1);

	while (typeTableEntries[slot].hash) {
		if (++slot == typeTableCapacity) slot = 0;
	}

	typeTableEntries[slot].value = type;
	typeTableEntries[slot].hash = type->hash;
}

void addStruct(TypeStruct *struct_) {
	PROFILE_FUNC();
	if (!struct_->hash) {
		if (nextStructHash == 0) nextStructHash = 1;
		u32 hash = nextStructHash;
		nextStructHash *= STRUCT_HASH_PRIME;

		struct_->hash = hash;

		insertIntoTable(struct_);
	}
}

TypePointer *getPointer(Type *type) {
	PROFILE_FUNC();
	u32 hash = (type->hash + 1) * POINTER_HASH_PRIME;
	if (hash == 0) hash = 1;

	u32 slot = hash & (typeTableCapacity - 1);

	for (TypeTableEntry entry = typeTableEntries[slot]; entry.hash; entry = typeTableEntries[slot]) {
		if (entry.hash == hash && entry.value->flavor == TypeFlavor::POINTER && static_cast<TypePointer *>(entry.value)->pointerTo == type) {
			return static_cast<TypePointer *>(entry.value);
		}

		if (++slot == typeTableCapacity) slot = 0;
	}

	auto result = TYPE_NEW(TypePointer);
	result->size = 8;
	result->alignment = 8;
	result->pointerTo = type;
	result->hash = hash;
	result->flavor = TypeFlavor::POINTER;

	result->name.length = type->name.length + 1;
	result->name.characters = TYPE_NEW_ARRAY(char, result->name.length);

	result->name.characters[0] = '*';
	memcpy(result->name.characters + 1, type->name.characters, type->name.length);

	insertIntoTable(result, slot);

	return result;
}

// The enclosingScope for these declarations will be incorrect but that shouldn't matter @Volatile
ExprLiteral *unsignedLiteralType;
Declaration *countDeclaration;
Declaration *capacityDeclaration;

Declaration *createDataDeclaration(Type *type) {
	PROFILE_FUNC();

	auto dataType = TYPE_NEW(ExprLiteral);
	dataType->flavor = ExprFlavor::TYPE_LITERAL;
	dataType->start = {};
	dataType->end = {};
	dataType->type = &TYPE_TYPE;
	dataType->typeValue = getPointer(type);

	auto pointerZero = TYPE_NEW(ExprLiteral);
	pointerZero->flavor = ExprFlavor::INT_LITERAL;
	pointerZero->start = {};
	pointerZero->end = {};
	pointerZero->unsignedValue = 0;
	pointerZero->type = dataType->typeValue;

	auto dataDeclaration = TYPE_NEW(Declaration);
	dataDeclaration->start = {};
	dataDeclaration->end = {};
	dataDeclaration->name = "data";
	dataDeclaration->type = dataType;
	dataDeclaration->initialValue = pointerZero;
	dataDeclaration->physicalStorage = 0;
	dataDeclaration->flags |= DECLARATION_TYPE_IS_READY | DECLARATION_VALUE_IS_READY;

	return dataDeclaration;
}

TypeArray *getArray(Type *type) {
	PROFILE_FUNC();
	u32 hash = (type->hash + 1) * ARRAY_HASH_PRIME;
	if (hash == 0) hash = 1;

	u32 slot = hash & (typeTableCapacity - 1);

	for (TypeTableEntry entry = typeTableEntries[slot]; entry.hash; entry = typeTableEntries[slot]) {
		if (entry.hash == hash && entry.value->flavor == TypeFlavor::ARRAY) {
			auto array = static_cast<TypeArray *>(entry.value);
			
			if (array->flags == 0 && array->arrayOf == type) {
				return array;
			}
		}

		if (++slot == typeTableCapacity) slot = 0;
	}

	auto result = TYPE_NEW(TypeArray);
	result->size = 16;
	result->alignment = 8;
	result->arrayOf = type;
	result->hash = hash;
	result->count = 0;
	result->flavor = TypeFlavor::ARRAY;
	insertIntoTable(result, slot);
	addDeclarationToBlockUnchecked(&result->members, createDataDeclaration(type), nullptr, 0);
	addDeclarationToBlockUnchecked(&result->members, countDeclaration, nullptr, 0);

	result->members.queued = true;
	result->members.flavor = BlockFlavor::STRUCT;


	result->name.length = type->name.length + 2;
	result->name.characters = TYPE_NEW_ARRAY(char, result->name.length);

	result->name.characters[0] = '[';
	result->name.characters[1] = ']';
	memcpy(result->name.characters + 2, type->name.characters, type->name.length);

	return result;
}

TypeArray *getDynamicArray(Type *type) {
	PROFILE_FUNC();
	u32 hash = (type->hash + 1) * DYNAMIC_ARRAY_HASH_PRIME;
	if (hash == 0) hash = 1;

	u32 slot = hash & (typeTableCapacity - 1);

	for (TypeTableEntry entry = typeTableEntries[slot]; entry.hash; entry = typeTableEntries[slot]) {
		if (entry.hash == hash && entry.value->flavor == TypeFlavor::ARRAY) {
			auto array = static_cast<TypeArray *>(entry.value);

			if (array->flags == TYPE_ARRAY_IS_DYNAMIC && array->arrayOf == type) {
				return array;
			}
		}

		if (++slot == typeTableCapacity) slot = 0;
	}

	auto result = TYPE_NEW(TypeArray);
	result->size = 24;
	result->alignment = 8;
	result->arrayOf = type;
	result->hash = hash;
	result->count = 0;
	result->flavor = TypeFlavor::ARRAY;
	result->flags |= TYPE_ARRAY_IS_DYNAMIC;
	insertIntoTable(result, slot);
	addDeclarationToBlockUnchecked(&result->members, createDataDeclaration(type), nullptr, 0);
	addDeclarationToBlockUnchecked(&result->members, countDeclaration, nullptr, 0);
	addDeclarationToBlockUnchecked(&result->members, capacityDeclaration, nullptr, 0);

	result->members.queued = true;
	result->members.flavor = BlockFlavor::STRUCT;

	result->name.length = type->name.length + 4;
	result->name.characters = TYPE_NEW_ARRAY(char, result->name.length);

	result->name.characters[0] = '[';
	result->name.characters[1] = '.';
	result->name.characters[2] = '.';
	result->name.characters[3] = ']';
	memcpy(result->name.characters + 4, type->name.characters, type->name.length);


	return result;
}

TypeArray *getStaticArray(Type *type, u32 count) {
	PROFILE_FUNC();
	u32 hash = type->hash * STATIC_ARRAY_TYPE_HASH_PRIME + count * STATIC_ARRAY_SIZE_HASH_PRIME;
	if (hash == 0) hash = 1;

	u32 slot = hash & (typeTableCapacity - 1);

	for (TypeTableEntry entry = typeTableEntries[slot]; entry.hash; entry = typeTableEntries[slot]) {
		if (entry.hash == hash && entry.value->flavor == TypeFlavor::ARRAY) {
			auto array = static_cast<TypeArray *>(entry.value);

			if (array->flags == TYPE_ARRAY_IS_FIXED && array->count == count && array->arrayOf == type) {
				return array;
			}
		}

		if (++slot == typeTableCapacity) slot = 0;
	}

	auto result = TYPE_NEW(TypeArray);
	result->size = type->size * count;
	result->alignment = type->alignment;
	result->arrayOf = type;
	result->hash = hash;
	result->count = count;
	result->flavor = TypeFlavor::ARRAY;
	result->flags |= TYPE_ARRAY_IS_FIXED;

	insertIntoTable(result, slot);

	addDeclarationToBlockUnchecked(&result->members, createDataDeclaration(type), nullptr, 0);

	ExprLiteral *countLiteral = TYPE_NEW(ExprLiteral);
	countLiteral->flavor = ExprFlavor::INT_LITERAL;
	countLiteral->start = {};
	countLiteral->end = {};
	countLiteral->unsignedValue = count;
	countLiteral->type = &TYPE_UNSIGNED_INT_LITERAL;

	auto countDeclaration = TYPE_NEW(Declaration);
	countDeclaration->start = {};
	countDeclaration->end = {};
	countDeclaration->name = "count";
	countDeclaration->type = unsignedLiteralType;
	countDeclaration->initialValue = countLiteral;
	countDeclaration->flags |= DECLARATION_IS_CONSTANT | DECLARATION_TYPE_IS_READY | DECLARATION_VALUE_IS_READY;
	addDeclarationToBlockUnchecked(&result->members, countDeclaration, nullptr, 0);

	assert(countDeclaration->type);

	result->members.queued = true;
	result->members.flavor = BlockFlavor::STRUCT;

	constexpr u64 maxCount = getDigitCount(UINT64_MAX);

	result->name.characters = TYPE_NEW_ARRAY(char, type->name.length + 2 + maxCount);

	result->name.characters[0] = '[';
	
	char *buffer = result->name.characters + 1;

	_ui64toa(count, buffer, 10);
	buffer = strchr(buffer, 0);
	buffer[0] = ']';

	memcpy(buffer + 1, type->name.characters, type->name.length);

	result->name.length = static_cast<u32>((buffer - result->name.characters) + 1 + type->name.length);

	return result;
}


void generateTypeNameForFunction(TypeFunction *function) {
	PROFILE_FUNC();
	function->name.length = 6; // "() -> "

	if (function->returnCount != 1) {
		function->name.length += 2;
	}

	if (function->flags & TYPE_FUNCTION_IS_C_CALL) {
		function->name.length += 8; // " #c_call"
	}

	for (u64 i = 0; i < function->returnCount; i++) {
		function->name.length += function->returnTypes[i]->name.length;

		if (i + 1 != function->returnCount) {
			function->name.length += 2;
		}
	}

	for (u64 i = 0; i < function->argumentCount; i++) {
		function->name.length += function->argumentTypes[i]->name.length;

		if (i + 1 != function->argumentCount) {
			function->name.length += 2;
		}
	}

	function->name.characters = TYPE_NEW_ARRAY(char, function->name.length);

	char *cursor = function->name.characters;
	*cursor++ = '(';

	for (u64 i = 0; i < function->argumentCount; i++) {
		String name = function->argumentTypes[i]->name;

		if (i == function->argumentCount - 1 && function->isVarargs) {
			*cursor++ = '.';
			*cursor++ = '.';

			assert(name.characters[0] == '[' && name.characters[1] == ']');

			memcpy(cursor, name.characters + 2, name.length - 2);
			cursor += name.length - 2;
		}
		else {
			memcpy(cursor, name.characters, name.length);
			cursor += name.length;
		}

		if (i + 1 != function->argumentCount) {
			*cursor++ = ',';
			*cursor++ = ' ';
		}
	}

	*cursor++ = ')';
	*cursor++ = ' ';
	*cursor++ = '-';
	*cursor++ = '>';
	*cursor++ = ' ';

	if (function->returnCount != 1) {
		*cursor++ = '(';
	}


	for (u64 i = 0; i < function->returnCount; i++) {
		String name = function->returnTypes[i]->name;

		memcpy(cursor, name.characters, name.length);

		cursor += name.length;

		if (i + 1 != function->returnCount) {
			*cursor++ = ',';
			*cursor++ = ' ';
		}
	}

	if (function->returnCount != 1) {
		*cursor++ = ')';
	}

	if (function->flags & TYPE_FUNCTION_IS_C_CALL) {
		memcpy(cursor, " #c_call", 8);
		cursor += 8;
	}
}

TypeFunction *getFunctionType(ExprFunction *expr) {
	PROFILE_FUNC();
	auto &arguments = expr->arguments.declarations;
	auto &returns = expr->returns.declarations;

	u32 hash = 0;

	for (auto return_ : returns) {
		hash += getDeclarationType(return_)->hash;
		hash *= FUNCTION_RETURN_HASH_PRIME;
	}


	for (auto argument : arguments) {
		hash += getDeclarationType(argument)->hash;
		hash *= FUNCTION_ARGUMENT_HASH_PRIME;
	}

	bool varags = expr->arguments.declarations.count && (expr->arguments.declarations[expr->arguments.declarations.count - 1]->flags & DECLARATION_IS_VARARGS) != 0;

	if (expr->flags & EXPR_FUNCTION_IS_C_CALL) {
		hash *= C_CALL_HASH_PRIME;
	}

	if (expr->flags & EXPR_FUNCTION_IS_COMPILER) {
		hash *= COMPILER_HASH_PRIME;
	}

	if (varags) {
		hash *= VARARGS_HASH_PRIME;
	}

	if (hash == 0) hash = 1;

	u32 slot = hash & (typeTableCapacity - 1);

	for (TypeTableEntry entry = typeTableEntries[slot]; entry.hash; entry = typeTableEntries[slot]) {
		if (entry.hash == hash && entry.value->flavor == TypeFlavor::FUNCTION) {
			auto function = static_cast<TypeFunction *>(entry.value);

			if (function->isVarargs != varags) {
				goto cont;
			}

			if (((function->flags & TYPE_FUNCTION_IS_C_CALL) != 0) != ((expr->flags & EXPR_FUNCTION_IS_C_CALL) != 0)) {
				goto cont;
			}

			if (function->argumentCount == arguments.count && function->returnCount == returns.count) {
				for (u32 i = 0; i < arguments.count; i++) {
					if (function->argumentTypes[i] != getDeclarationType(arguments[i])) {
						goto cont;
					}
				}

				for (u32 i = 0; i < returns.count; i++) {
					if (function->returnTypes[i] != getDeclarationType(returns[i])) {
						goto cont;
					}
				}

				return function;
			}
		}

		cont:
		if (++slot == typeTableCapacity) slot = 0;
	}

	auto result = TYPE_NEW(TypeFunction);
	result->size = 8;
	result->alignment = 8;
	result->returnCount = returns.count;
	result->returnTypes = TYPE_NEW_ARRAY(Type *, result->returnCount);
	result->hash = hash;
	result->argumentCount = arguments.count;
	result->argumentTypes = TYPE_NEW_ARRAY(Type *, result->argumentCount);
	result->isVarargs = varags;

	if (expr->flags & EXPR_FUNCTION_IS_C_CALL) {
		result->flags |= TYPE_FUNCTION_IS_C_CALL;
	}

	for (u32 i = 0; i < returns.count; i++) {
		result->returnTypes[i] = getDeclarationType(returns[i]);
	}

	for (u32 i = 0; i < arguments.count; i++) {
		result->argumentTypes[i] = getDeclarationType(arguments[i]);
	}

	result->flavor = TypeFlavor::FUNCTION;
	generateTypeNameForFunction(result);

	insertIntoTable(result, slot);

	return result;
}

void setupTypeTable() {
	PROFILE_FUNC();
	typeTableCapacity = 1024;
	typeTableEntries = new TypeTableEntry[typeTableCapacity];
	count = 0;

	insertIntoTable(&TYPE_VOID);

	insertIntoTable(&TYPE_U8);
	insertIntoTable(&TYPE_U16);
	insertIntoTable(&TYPE_U32);
	insertIntoTable(&TYPE_U64);

	insertIntoTable(&TYPE_S8);
	insertIntoTable(&TYPE_S16);
	insertIntoTable(&TYPE_S32);
	insertIntoTable(&TYPE_S64);

	insertIntoTable(&TYPE_F32);
	insertIntoTable(&TYPE_F64);

	insertIntoTable(&TYPE_BOOL);
	insertIntoTable(&TYPE_STRING);
	insertIntoTable(&TYPE_TYPE);

	auto u64Type = TYPE_NEW(ExprLiteral);
	u64Type->flavor = ExprFlavor::TYPE_LITERAL;
	u64Type->start = {};
	u64Type->end = {};
	u64Type->type = &TYPE_TYPE;
	u64Type->typeValue = &TYPE_U64;

	unsignedLiteralType = TYPE_NEW(ExprLiteral);
	unsignedLiteralType->flavor = ExprFlavor::TYPE_LITERAL;
	unsignedLiteralType->start = {};
	unsignedLiteralType->end = {};
	unsignedLiteralType->type = &TYPE_TYPE;
	unsignedLiteralType->typeValue = &TYPE_UNSIGNED_INT_LITERAL;

	auto u64Zero = TYPE_NEW(ExprLiteral);
	u64Zero->flavor = ExprFlavor::INT_LITERAL;
	u64Zero->start = {};
	u64Zero->end = {};
	u64Zero->unsignedValue = 0;
	u64Zero->type = &TYPE_U64;

	countDeclaration = TYPE_NEW(Declaration);
	countDeclaration->start = {};
	countDeclaration->end = {};
	countDeclaration->name = "count";
	countDeclaration->type = u64Type;
	countDeclaration->initialValue = u64Zero;
	countDeclaration->physicalStorage = 8;
	countDeclaration->flags |= DECLARATION_TYPE_IS_READY | DECLARATION_VALUE_IS_READY;

	capacityDeclaration = TYPE_NEW(Declaration);
	capacityDeclaration->start = {};
	capacityDeclaration->end = {};
	capacityDeclaration->name = "capacity";
	capacityDeclaration->type = u64Type;
	capacityDeclaration->initialValue = u64Zero;
	capacityDeclaration->physicalStorage = 16;
	capacityDeclaration->flags |= DECLARATION_TYPE_IS_READY | DECLARATION_VALUE_IS_READY;

	addDeclarationToBlockUnchecked(&TYPE_STRING.members, createDataDeclaration(&TYPE_U8), nullptr, 0);
	addDeclarationToBlockUnchecked(&TYPE_STRING.members, countDeclaration, nullptr, 0);

	TYPE_VOID_POINTER = getPointer(&TYPE_VOID);
	TYPE_U8_POINTER = getPointer(&TYPE_U8);
	TYPE_U8_ARRAY = getArray(&TYPE_U8);
}