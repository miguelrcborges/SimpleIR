#ifndef SIR_H

#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

static inline uint32_t __builtin_ctz(uint32_t x) {
	unsigned long ret;
	_BitScanForward(&ret, x);
	return (uint32_t)ret;
}

#define _Thread_local __declspec(thread)
#endif


typedef ptrdiff_t Size;

enum SIR_Instruction {
	SIR_Add,
	SIR_Sub,
	SIR_SMul,
	SIR_SDiv,
	SIR_SMod,
	SIR_UMul,
	SIR_UDiv,
	SIR_UMod,

	SIR_And,
	SIR_Or,
	SIR_Xor,
	SIR_SShl,
	SIR_SShr,
	SIR_UShl,
	SIR_USHr,

	SIR_CmpEq,
	SIR_CmpNeq,
	SIR_CmpULow,
	SIR_CmpULowEq,
	SIR_CmpUGt,
	SIR_CmpUGtEq,
	SIR_CmpSLow,
	SIR_CmpSLowEq,
	SIR_CmpSGt,
	SIR_CmpSGtEq,

	SIR_ReadFromAddr,
	SIR_WriteToAddr,
	SIR_Call,
	SIR_Alloc,
	SIR_Set,

	SIR_Br,
	SIR_BrIf,
	SIR_Phi,
	SIR_Ret,

	SIR_Instruction_Count
};

typedef enum AMD64_CallingConventions {
	AMD64_SYSV,
	AMD64_WIN,

	AMD64_CallingConventionsCount
} AMD64_CallingConventions;

#define SIR_OperandTypeMask 0b00001111
enum SIR_InstructionOperandType { SIR_Var, SIR_Immediate, SIR_Constant };

#define SIR_InstructionWidthOffset 4
#define SIR_InstructionWidthMask 0b11110000
enum SIR_InstructionWidth {
	SIR_QWORD = 0 << SIR_InstructionWidthOffset,
	SIR_DWORD = 1 << SIR_InstructionWidthOffset,
	SIR_WORD = 2 << SIR_InstructionWidthOffset,
	SIR_BYTE = 3 << SIR_InstructionWidthOffset,

	SIR_InstructionWidthCount
};

typedef struct SIR_Operation {
	uint8_t Instruction;
	uint8_t InstructionOptions;
	uint16_t OperandW1;
	union {
		struct {
			uint16_t OperandW2;
			uint16_t OperandW3;
		};
		uint32_t OperandDW2;
	};
} SIR_Operation;

typedef struct SIR_Function {
	void **FunctionPointerToOverride;
	SIR_Operation *Operations;
	Size OperationsCount;
	Size ArgumentsCount;
	Size ReturnCount;
} SIR_Function;

void SIR_AMD64Compile(SIR_Function *Functions, Size FunctionsCount, void *OutputExecutableMemory, Size OutputExecutableMemorySize,
							 void *OutputReadOnlyMemory, Size OutputReadOnlyMemorySize, uint64_t *Constants, AMD64_CallingConventions Convention);

#define SIR_H
#endif
