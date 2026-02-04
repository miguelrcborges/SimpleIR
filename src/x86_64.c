#include <sir.h>

#include <assert.h>
#include <string.h>

enum Regs {
	RAX = 1,
	RBX,
	RCX,
	RDX,
	RSI,
	RDI,
	R8,
	R9,
	R10,
	R11,
	R12,
	R13,
	R14,
	R15,

	Regs_Count
};

static const uint8_t RegistersEnconding[Regs_Count] = {
	 [RAX] = 0b000, [RBX] = 0b011, [RCX] = 0b001, [RDX] = 0b010, [RSI] = 0b110, [RDI] = 0b111, [R8] = 0b000,
	 [R9] = 0b001,	 [R10] = 0b010, [R11] = 0b011, [R12] = 0b100, [R13] = 0b101, [R14] = 0b110, [R15] = 0b111};

typedef struct AMD64CompileContext {
	int16_t VarsLocation[4096];
	int16_t MemFreeStack[4096];
	uint8_t *restrict ExecutableMemory;
	Size ExecutableMemoryCursor;
	Size CurrentlyFreed;
	int16_t CurrentRegsVar[Regs_Count];
	uint32_t FreeRegs;
	int32_t MemStackAllocated;
	int16_t ForceOutReg;
	int16_t MemFreeStackCursor;
} AMD64CompileContext;

#define WriteByte(b)                                                                                                                       \
	do {                                                                                                                                    \
		c->ExecutableMemoryCursor -= 1;                                                                                                      \
		assert(c->ExecutableMemoryCursor >= 0);                                                                                              \
		c->ExecutableMemory[c->ExecutableMemoryCursor] = b;                                                                                  \
	} while (0);

static void SIR_AMD64WriteRM(AMD64CompileContext *c, uint8_t RegSection, Size RMTarget, uint8_t Width) {
	uint8_t RMMod = (RMTarget << 3) < INT8_MIN ? 0b10 : 0b01;
	RMMod = RMTarget > 0 ? 0b11 : RMMod;
	uint8_t RMRm = RMTarget > 0 ? RegistersEnconding[RMTarget] : 0b101;
	uint8_t RMByte = (RMMod << 6) | (RegSection << 3) | (RMRm);
	int32_t MemOffset = (int32_t)(RMTarget << 3);
	int DisplacementBytes = Width == SIR_WORD ? 2 : 4;
	DisplacementBytes = (Width == SIR_BYTE || RMMod == 0b01) ? 1 : DisplacementBytes;
	DisplacementBytes = RMMod == 0b11 ? -1 : DisplacementBytes;

	for (int i = DisplacementBytes - 1; i >= 0; i -= 1) {
		uint8_t Byte = (uint8_t)((uint64_t)MemOffset >> (8 * i));
		WriteByte(Byte);
	};
	WriteByte(RMByte);
}

static void SIR_AMD64WriteMov(AMD64CompileContext *c, Size Reg, Size RegOrMem, uint8_t Width, int TrueIfToReg) {
	uint8_t OpCode = TrueIfToReg ? 0x8B : 0x89;
	OpCode = Width == SIR_BYTE ? OpCode - 1 : OpCode;

	SIR_AMD64WriteRM(c, RegistersEnconding[Reg], RegOrMem, Width);
	WriteByte(OpCode);
	uint8_t Rex = Width == SIR_QWORD ? 0b01001000 : 0b01000000;
	Rex |= RegOrMem >= R8 ? 0b001 : 0;
	Rex |= Reg >= R8 ? 0b100 : 0;
	if (Rex != 0b01000000) {
		WriteByte(Rex);
	}
	if (Width == SIR_WORD) {
		WriteByte(0x67);
	}
}

static void SIR_AMD64PushPopReg(AMD64CompileContext *c, Size Reg, int TrueIfPush) {
	uint8_t OpCode = TrueIfPush ? 0x50 : 0x58;
	OpCode |= RegistersEnconding[Reg];
	WriteByte(OpCode);

	if (Reg >= R8) {
		uint8_t Rex = 0b0100000;
		Rex |= 1;
		WriteByte(Rex);
	}
}

static void SIR_AMD64WriteExitSequence(AMD64CompileContext *c) {
	WriteByte(0xC3); // ret
	WriteByte(0x5D); // pop rbp
}

static Size SIR_AMD64GetVarIntoReg(AMD64CompileContext *c, Size Var, uint32_t DoNotForceOutThisMask) {
	Size InitialLoc = c->VarsLocation[Var];
	if (InitialLoc > 0)
		return InitialLoc;

	// Try to reuse current op reg to avoid a move
	uint32_t FreeReg = (c->FreeRegs & (1 << c->CurrentlyFreed)) ? c->CurrentlyFreed : 31;
	FreeReg = FreeReg == 31 ? __builtin_ctz(c->FreeRegs) : FreeReg;

	// No free regs, push one reg to the stack
	if (FreeReg == 31) {
		// TODO: Maybe improve reg selection
		do {
			FreeReg = c->ForceOutReg;
			c->ForceOutReg = (c->ForceOutReg + 1 >= Regs_Count) ? RAX : (c->ForceOutReg + 1);
		} while ((DoNotForceOutThisMask & (1u << FreeReg)) != 0);
		int16_t VarToPush = c->CurrentRegsVar[FreeReg];
		int32_t MemOffset;
		if (c->MemFreeStackCursor > 0) {
			c->MemFreeStackCursor -= 1;
			MemOffset = 8 * c->MemFreeStack[c->MemFreeStackCursor];
		} else {
			c->MemStackAllocated -= 8;
			c->MemStackAllocated &= ~7u;
			MemOffset = c->MemStackAllocated;
		}
		Size MemIndex = MemOffset >> 3;
		c->VarsLocation[VarToPush] = MemIndex;
		SIR_AMD64WriteMov(c, FreeReg, MemIndex, SIR_QWORD, 1);
	}

	c->VarsLocation[Var] = FreeReg;
	c->FreeRegs &= ~(1 << FreeReg);
	c->CurrentRegsVar[FreeReg] = Var;

	if (InitialLoc < 0) {
		SIR_AMD64WriteMov(c, FreeReg, InitialLoc, SIR_QWORD, 0);
	}

	return FreeReg;
}

#undef WriteByte

void SIR_AMD64Compile(SIR_Function *Functions, Size FunctionsCount, void *OutputExecutableMemory, Size OutputExecutableMemorySize,
							 void *OutputReadOnlyMemory, Size OutputReadOnlyMemorySize, uint64_t *Constants, AMD64_CallingConventions Convention) {
	AMD64CompileContext c;
	c.ExecutableMemoryCursor = OutputExecutableMemorySize;
	c.ExecutableMemory = (uint8_t *)OutputExecutableMemory;

#define WriteByte(b)                                                                                                                       \
	do {                                                                                                                                    \
		c.ExecutableMemoryCursor -= 1;                                                                                                       \
		assert(c.ExecutableMemoryCursor >= 0);                                                                                               \
		c.ExecutableMemory[c.ExecutableMemoryCursor] = b;                                                                                    \
	} while (0);

	for (Size i = 0; i < FunctionsCount; i += 1) {
		SIR_Function *f = &Functions[i];
		memset(c.VarsLocation, 0, sizeof(c.VarsLocation));
		memset(c.MemFreeStack, 0, sizeof(c.MemFreeStack));
		memset(c.CurrentRegsVar, 0, sizeof(c.CurrentRegsVar));
		c.ForceOutReg = RAX;

		c.FreeRegs = 1 << 31;
		for (int i = RAX; i <= R15; i += 1) {
			c.FreeRegs |= 1 << i;
		}

		uint32_t InsertReturn = 1;
		if (f->OperationsCount > 0 && f->Operations[f->OperationsCount - 1].Instruction == SIR_Ret) {
			InsertReturn = 0;
		}

		if (InsertReturn) {
			SIR_AMD64WriteExitSequence(&c);
		}

		for (Size op = f->OperationsCount - 1; op >= 0; op -= 1) {
			SIR_Operation *i = &f->Operations[op];
			c.CurrentlyFreed = c.VarsLocation[f->ArgumentsCount + op];

			if (c.CurrentlyFreed < 0) {
				c.MemFreeStack[c.MemFreeStackCursor] = c.CurrentlyFreed;
				c.MemFreeStackCursor += 1;
			} else if (c.CurrentlyFreed > 0) {
				c.FreeRegs |= (1 << c.CurrentlyFreed);
			}

			if (i->Instruction == SIR_Ret) {
				// TODO: Handle SysV && W=2 and W>2
				if (f->ReturnCount == 1) {
					Size ReturnVar = i->OperandW1;
					Size VarLocation = c.VarsLocation[ReturnVar];
					// TODO: Unhack this, even though since this should be the last instruction, element has to be free.
					// 	   Save RAX to another register/mem and reserve RAX to this.
					//       Returned Element not assigned. Expected case unless there is branches
					assert(VarLocation == 0);
					c.VarsLocation[ReturnVar] = RAX;
					c.FreeRegs &= ~(1 << RAX);
					c.CurrentRegsVar[RAX] = ReturnVar;
				}
				SIR_AMD64WriteExitSequence(&c);

			} else if (i->Instruction == SIR_Add || i->Instruction == SIR_Sub) {
				if (c.CurrentlyFreed == 0)
					// Never used, no need to codegen this
					continue;
				uint8_t OpType = i->InstructionOptions & SIR_OperandTypeMask;
				uint8_t OpWidth = (i->InstructionOptions & SIR_InstructionWidthMask) >> SIR_InstructionWidthOffset;

				if (i->InstructionOptions == SIR_Var) {
					Size Op1 = i->OperandW1;
					Size Op2 = i->OperandW2;

					Size Op1Loc = SIR_AMD64GetVarIntoReg(&c, Op1, 0);
					Size Op2Loc = SIR_AMD64GetVarIntoReg(&c, Op2, 0);

					uint8_t Opcode = 0x01; // RM reg ADD
					Opcode = i->Instruction == SIR_Sub ? 0x2B : Opcode;
					SIR_AMD64WriteRM(&c, Op2Loc, c.CurrentlyFreed, OpWidth);
					WriteByte(Opcode);
					if (c.CurrentlyFreed != Op1Loc) {
						SIR_AMD64WriteMov(&c, Op1Loc, c.CurrentlyFreed, OpWidth, 0);
					}

				} else if (OpType == SIR_Immediate) {
					Size Op1 = i->OperandW1;
					uint32_t Val = i->OperandDW2;
					Size Op1Loc = SIR_AMD64GetVarIntoReg(&c, Op1, 0);

					int NImmBytes = OpWidth <= SIR_DWORD ? 4 : 4 - OpWidth;
					for (int ByteIdx = NImmBytes - 1; ByteIdx >= 0; ByteIdx -= 1) {
						uint8_t Byte = (uint8_t)(Val >> (ByteIdx * 8));
						WriteByte(Byte);
					}

					// Main Op
					uint8_t RMReg = 0b000;
					RMReg = i->Instruction == SIR_Sub ? 0b101 : RMReg;
					SIR_AMD64WriteRM(&c, RMReg, c.CurrentlyFreed, OpWidth);
					uint8_t OpCode = OpWidth == SIR_BYTE ? 0x80 : 0x81;
					WriteByte(OpCode);
					uint8_t Rex = OpWidth == SIR_QWORD ? 0b01001000 : 0b01000000;
					Rex |= c.CurrentlyFreed >= R8 ? 0b001 : 0;
					if (Rex != 0b01000000) {
						WriteByte(Rex);
					}
					if (OpWidth == SIR_WORD) {
						WriteByte(0x67);
					}

					if (c.CurrentlyFreed != Op1Loc) {
						SIR_AMD64WriteMov(&c, Op1Loc, c.CurrentlyFreed, OpWidth, 0);
					}
				}
			}
		}

		Size *InputRegisters = Convention == AMD64_WIN ? (Size[]){RCX, RDX, R8, R9} : (Size[]){RDI, RSI, RDX, RCX, R8, R9};
		Size NInputRegisters = Convention == AMD64_WIN ? 4 : 6;
		for (int i = 0; i < f->ArgumentsCount; i += 1) {
			if (i < NInputRegisters) {
				Size TargetLocation = InputRegisters[i];
				Size CurrentLocation = c.VarsLocation[i];
				if (TargetLocation != CurrentLocation) {
					if (c.CurrentRegsVar[TargetLocation] != 0) {
						// Stack is now free to use, so use it!
						SIR_AMD64WriteMov(&c, TargetLocation, -1, 0, 1);
					}
					SIR_AMD64WriteMov(&c, TargetLocation, CurrentLocation, 0, 0);
					if (c.CurrentRegsVar[TargetLocation] != 0) {
						SIR_AMD64WriteMov(&c, TargetLocation, -1, 0, 0);
					}
				}

				c.VarsLocation[i] = 0;
				c.FreeRegs |= 1 << TargetLocation;
				c.CurrentRegsVar[TargetLocation] = 0;
			} else {
				// TODO: Implement
				assert(0);
			}
		}

		// mov rbp, rsp
		WriteByte(0xE5);
		WriteByte(0x89);
		WriteByte(0x48);
		// push rbp
		WriteByte(0x55);
		*f->FunctionPointerToOverride = &c.ExecutableMemory[c.ExecutableMemoryCursor];
	}

#undef WriteByte
}
