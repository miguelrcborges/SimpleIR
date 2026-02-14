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
	int16_t VarsLocation[65565];
	int16_t MemFreeStack[4096];
	uint8_t *restrict ExecutableMemory;
	Size *CalleeSavedRegisters;
	Size NCalleeSavedRegisters;
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

static void SIR_AMD64ForceRegToMem(AMD64CompileContext *c, Size Reg) {
	if (Reg <= 0)
		return;
	if (c->CurrentRegsVar[Reg] == 0)
		return;
	Size MemIndex;
	if (c->MemFreeStackCursor > 0) {
		c->MemFreeStackCursor -= 1;
		MemIndex = 8 * c->MemFreeStack[c->MemFreeStackCursor];
	} else {
		c->MemStackAllocated -= 8;
		c->MemStackAllocated &= ~7u;
		MemIndex = c->MemStackAllocated >> 3;
	}

	Size Var = c->CurrentRegsVar[Reg];
	c->CurrentRegsVar[Reg] = 0;
	c->VarsLocation[Var] = MemIndex;
	c->FreeRegs |= 1 << Reg;
	SIR_AMD64WriteMov(c, Reg, MemIndex, 0, 1);
}

static void SIR_AMD64PushPopReg(AMD64CompileContext *c, Size Reg, int TrueIfPush) {
	uint8_t OpCode = TrueIfPush ? 0x50 : 0x58;
	OpCode |= RegistersEnconding[Reg];
	WriteByte(OpCode);

	if (Reg >= R8) {
		uint8_t Rex = 0b01000001;
		WriteByte(Rex);
	}
}

static void SIR_AMD64WriteExitSequence(AMD64CompileContext *c) {
	WriteByte(0xC3); // ret
	for (Size i = 0; i < c->NCalleeSavedRegisters; i += 1) {
		SIR_AMD64PushPopReg(c, c->CalleeSavedRegisters[i], 0);
	}
	WriteByte(0x5D); // pop rbp
}

static Size SIR_AMD64GetVarIntoReg(AMD64CompileContext *c, Size Var, uint32_t DoNotUseThisMask) {
	Size InitialLoc = c->VarsLocation[Var];
	if (InitialLoc > 0)
		return InitialLoc;

	// Try to reuse current op reg to avoid a move
	uint32_t UsableFree = c->FreeRegs & (~DoNotUseThisMask);
	uint32_t FreeReg = (UsableFree & (1 << c->CurrentlyFreed)) ? c->CurrentlyFreed : 31;
	FreeReg = FreeReg == 31 ? __builtin_ctz(UsableFree) : FreeReg;

	// No free regs, push one reg to the stack
	if (FreeReg == 31) {
		// TODO: Maybe improve reg selection
		do {
			FreeReg = c->ForceOutReg;
			c->ForceOutReg = (c->ForceOutReg + 1 >= Regs_Count) ? RAX : (c->ForceOutReg + 1);
		} while ((DoNotUseThisMask & (1u << FreeReg)) != 0);
		int16_t VarToPush = c->CurrentRegsVar[FreeReg];
		Size MemIndex;
		if (c->MemFreeStackCursor > 0) {
			c->MemFreeStackCursor -= 1;
			MemIndex = c->MemFreeStack[c->MemFreeStackCursor];
		} else {
			c->MemStackAllocated -= 8;
			c->MemStackAllocated &= ~7u;
			MemIndex = c->MemStackAllocated >> 3;
		}
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

void SIR_AMD64Compile(SIR_Function *Functions, Size FunctionsCount, void *OutputExecutableMemory, Size OutputExecutableMemorySize,
							 void *OutputReadOnlyMemory, Size OutputReadOnlyMemorySize, uint64_t *Constants, AMD64_CallingConventions Convention) {
	_Thread_local static AMD64CompileContext ctx;
	AMD64CompileContext *c = &ctx;
	c->ExecutableMemoryCursor = OutputExecutableMemorySize;
	c->ExecutableMemory = (uint8_t *)OutputExecutableMemory;

	// TODO: Implement constant arguments, but Im still thinking on how these can be used nicely
	// TODO: Optimize immediates that could be written in a single byte.
	for (Size i = 0; i < FunctionsCount; i += 1) {
		SIR_Function *f = &Functions[i];
		memset(c->VarsLocation, 0, sizeof(c->VarsLocation[0] * (f->OperationsCount + f->ArgumentsCount)));
		memset(c->MemFreeStack, 0, sizeof(c->MemFreeStack));
		memset(c->CurrentRegsVar, 0, sizeof(c->CurrentRegsVar));
		c->MemStackAllocated = 0;
		c->MemFreeStackCursor = 0;
		c->ForceOutReg = RAX;

		c->FreeRegs = 1u << 31;
		for (int i = RAX; i <= R15; i += 1) {
			c->FreeRegs |= 1 << i;
		}

		c->CalleeSavedRegisters = Convention == AMD64_WIN ? (Size[]){RBX, RDI, RSI, R12, R13, R14, R15} : (Size[]){RBX, R12, R13, R14, R15};
		c->NCalleeSavedRegisters = Convention == AMD64_WIN ? 7 : 5;

		uint32_t InsertReturn = 1;
		if (f->OperationsCount > 0 && f->Operations[f->OperationsCount - 1].Instruction == SIR_Ret) {
			InsertReturn = 0;
		}

		if (InsertReturn) {
			SIR_AMD64WriteExitSequence(c);
		}

		for (Size op = f->OperationsCount - 1; op >= 0; op -= 1) {
			SIR_Operation *i = &f->Operations[op];
			Size ThisVar = f->ArgumentsCount + op;
			c->CurrentlyFreed = c->VarsLocation[ThisVar];

			if (c->CurrentlyFreed < 0) {
				c->MemFreeStack[c->MemFreeStackCursor] = c->CurrentlyFreed;
				c->MemFreeStackCursor += 1;
			} else if (c->CurrentlyFreed > 0) {
				c->FreeRegs |= (1 << c->CurrentlyFreed);
				c->CurrentRegsVar[c->CurrentlyFreed] = 0;
			}

			// Dead Code Elemination
			if (c->CurrentlyFreed == 0 && (i->Instruction != SIR_Ret && i->Instruction != SIR_Call && i->Instruction != SIR_WriteToAddr))
				continue;

			uint8_t OpType = i->InstructionOptions & SIR_OperandTypeMask;
			uint8_t OpWidth = (i->InstructionOptions & SIR_InstructionWidthMask) >> SIR_InstructionWidthOffset;

			if (i->Instruction == SIR_Ret) {
				// TODO: Handle SysV && W=2 and W>2
				if (f->ReturnCount == 1) {
					Size ReturnVar = i->OperandW1;
					Size VarLocation = c->VarsLocation[ReturnVar];
					// TODO: Unhack this, even though since this should be the last instruction, element has to be free.
					// 	   Save RAX to another register/mem and reserve RAX to this.
					//       Returned Element not assigned. Expected case unless there is branches
					assert(VarLocation == 0);
					c->VarsLocation[ReturnVar] = RAX;
					c->FreeRegs &= ~(1 << RAX);
					c->CurrentRegsVar[RAX] = ReturnVar;
				}
				SIR_AMD64WriteExitSequence(c);

			} else if (i->Instruction == SIR_Add || i->Instruction == SIR_Sub) {
				Size Op1 = i->OperandW1;
				Size Op1Loc = SIR_AMD64GetVarIntoReg(c, Op1, 0);

				if (OpType == SIR_Var) {
					Size Op2 = i->OperandW2;
					Size Op2Loc = SIR_AMD64GetVarIntoReg(c, Op2, 0);

					uint8_t Opcode = 0x01; // RM reg ADD
					Opcode = i->Instruction == SIR_Sub ? 0x2B : Opcode;
					SIR_AMD64WriteRM(c, Op2Loc, c->CurrentlyFreed, OpWidth);
					WriteByte(Opcode);

				} else if (OpType == SIR_Immediate) {
					uint32_t Val = i->OperandDW2;

					int NImmBytes = OpWidth <= SIR_DWORD ? 4 : 4 - OpWidth;
					for (int ByteIdx = NImmBytes - 1; ByteIdx >= 0; ByteIdx -= 1) {
						uint8_t Byte = (uint8_t)(Val >> (ByteIdx * 8));
						WriteByte(Byte);
					}

					// Main Op
					uint8_t RMReg = 0b000;
					RMReg = i->Instruction == SIR_Sub ? 0b101 : RMReg;
					SIR_AMD64WriteRM(c, RMReg, c->CurrentlyFreed, OpWidth);
					uint8_t OpCode = OpWidth == SIR_BYTE ? 0x80 : 0x81;
					WriteByte(OpCode);
					uint8_t Rex = OpWidth == SIR_QWORD ? 0b01001000 : 0b01000000;
					Rex |= c->CurrentlyFreed >= R8 ? 0b001 : 0;
					if (Rex != 0b01000000) {
						WriteByte(Rex);
					}
					if (OpWidth == SIR_WORD) {
						WriteByte(0x67);
					}
				}

				if (c->CurrentlyFreed != Op1Loc) {
					SIR_AMD64WriteMov(c, Op1Loc, c->CurrentlyFreed, OpWidth, 0);
				}

			} else if (i->Instruction == SIR_SMul && OpWidth != SIR_BYTE) {
				Size FinalLocation = c->CurrentlyFreed;
				// Get this first in order to attemp to get RAX
				if (FinalLocation < 0) {
					FinalLocation = SIR_AMD64GetVarIntoReg(c, ThisVar, 0);
				}
				Size Op1 = i->OperandW1;
				Size Op1Loc = c->VarsLocation[Op1];
				if (Op1Loc == 0) {
					Op1Loc = SIR_AMD64GetVarIntoReg(c, Op1, 0);
				}
				uint8_t FinalLocationEnconding = RegistersEnconding[FinalLocation];

				if (OpType == SIR_Var) {
					Size Op2 = i->OperandW2;

					Size Op2Loc = c->VarsLocation[Op2];
					if (Op2Loc == 0) {
						Op2Loc = SIR_AMD64GetVarIntoReg(c, Op2, 0);
					}

					SIR_AMD64WriteRM(c, FinalLocationEnconding, Op2Loc, OpWidth);
					WriteByte(0xAF);
					WriteByte(0x0F);
					uint8_t Rex = OpWidth == SIR_QWORD ? 0b01001000 : 0b01000000;
					Rex |= FinalLocation >= R8 ? 0b100 : 0;
					Rex |= Op2Loc >= R8 ? 0b001 : 0;
					if (Rex != 0b01000000) {
						WriteByte(Rex);
					}
					if (OpWidth == SIR_WORD) {
						WriteByte(0x67);
					}

				} else if (OpType == SIR_Immediate) {
					uint32_t Val = i->OperandDW2;

					int NImmBytes = OpWidth <= SIR_DWORD ? 4 : 4 - OpWidth;
					for (int ByteIdx = NImmBytes - 1; ByteIdx >= 0; ByteIdx -= 1) {
						uint8_t Byte = (uint8_t)(Val >> (ByteIdx * 8));
						WriteByte(Byte);
					}

					SIR_AMD64WriteRM(c, FinalLocationEnconding, Op1Loc, OpWidth);
					WriteByte(0x69);
					uint8_t Rex = OpWidth == SIR_QWORD ? 0b01001000 : 0b01000000;
					Rex |= FinalLocation >= R8 ? 0b100 : 0;
					Rex |= Op1Loc >= R8 ? 0b001 : 0;
					if (Rex != 0b01000000) {
						WriteByte(Rex);
					}
					if (OpWidth == SIR_WORD) {
						WriteByte(0x67);
					}
				}

				if (Op1Loc != FinalLocation) {
					SIR_AMD64WriteMov(c, FinalLocation, Op1Loc, OpWidth, 1);
				}

				c->FreeRegs |= (1 << FinalLocation);

			} else if (i->Instruction >= SIR_SMul && i->Instruction <= SIR_UMod) {
				Size TargetLocation = (i->Instruction == SIR_SMod || i->Instruction == SIR_UMod) ? RDX : RAX;

				SIR_AMD64ForceRegToMem(c, RAX);
				SIR_AMD64ForceRegToMem(c, RDX);

				Size Op1 = i->OperandW1;
				Size Op1Loc = c->VarsLocation[Op1];
				if (Op1Loc == 0) {
					Op1Loc = SIR_AMD64GetVarIntoReg(c, Op1, 0);
				}

				// Write move from result location to location where result is used
				if (c->CurrentlyFreed != TargetLocation) {
					SIR_AMD64WriteMov(c, TargetLocation, c->CurrentlyFreed, OpWidth, 0);
				}

				// Intended Operation
				Size MulRm;
				Size Op2Loc;
				uint8_t Opcode = OpWidth == SIR_BYTE ? 0xF6 : 0xF7;
				uint8_t RegSection = 05; // IMUL
				RegSection = i->Instruction == SIR_UMul ? 04 : RegSection;
				RegSection = i->Instruction == SIR_SDiv || i->Instruction == SIR_SMod ? 07 : RegSection;
				RegSection = i->Instruction == SIR_UDiv || i->Instruction == SIR_UMod ? 06 : RegSection;
				if (OpType == SIR_Var) {
					Size Op2 = i->OperandW2;
					Op2Loc = c->VarsLocation[Op2];
					if (Op2Loc == 0) {
						Op2Loc = SIR_AMD64GetVarIntoReg(c, Op2, 0);
					}
				} else if (OpType == SIR_Immediate) {
					if (i->Instruction == SIR_SMul || i->Instruction == SIR_UMul) {
						// Use RDX since it is known to be free
						Op2Loc = RDX;
					} else {
						// TODO: Maybe improve here register freeing, change var allocation function to be able to
						// free regs without associating vars
						SIR_AMD64ForceRegToMem(c, RBX);
						Op2Loc = RBX;
					}
				}
				SIR_AMD64WriteRM(c, RegSection, Op2Loc, OpWidth);
				WriteByte(Opcode);
				uint8_t Rex = OpWidth == SIR_QWORD ? 0b01001000 : 0b01000000;
				Rex |= Op2Loc >= R8 ? 0b001 : 0;
				WriteByte(Rex);
				if (OpWidth == SIR_WORD) {
					WriteByte(0x67);
				}

				// Load to RAX the intended parameter value
				if (Op1Loc != RAX) {
					SIR_AMD64WriteMov(c, RAX, Op1Loc, OpWidth, 1);
				}

				if ((i->Instruction == SIR_UDiv || i->Instruction == SIR_UMod) && OpWidth != SIR_BYTE) {
					// xor edx, edx
					WriteByte(0xD2);
					WriteByte(0x31);
				} else if ((i->Instruction == SIR_SDiv || i->Instruction == SIR_SMod) && OpWidth != SIR_BYTE) {
					// c(q|w|d)o
					WriteByte(0x99);
					if (OpWidth == SIR_QWORD) {
						WriteByte(0b01000000);
					} else if (OpWidth == SIR_WORD) {
						WriteByte(0x67);
					}
				}

				// Load immediate or constant
				if (OpType == SIR_Immediate || OpType == SIR_Constant) {
					Size Immediate = OpType == SIR_Immediate ? (int32_t)i->OperandDW2 : Constants[i->OperandW1];
					uint8_t *ImmediateWidths = (uint8_t[]){8, 4, 2, 1};
					int ImmediateWidth = ImmediateWidths[OpWidth];
					for (int i = ImmediateWidth - 1; i >= 0; i -= 1) {
						uint8_t Byte = ((uint64_t)Immediate >> (i * 8));
						WriteByte(Byte);
					}
					uint8_t Opcode = OpWidth == SIR_BYTE ? 0xB0 : 0xB8;
					Opcode |= RegistersEnconding[Op2Loc];
					WriteByte(Opcode);
					uint8_t Rex = OpWidth == SIR_QWORD ? 0b01001000 : 0b01000000;
					WriteByte(Rex);
					if (OpWidth == SIR_WORD) {
						WriteByte(0x67);
					}
				}

			} else {
				assert(!"Unimplemented");
			}
		}

		Size *InputRegisters = Convention == AMD64_WIN ? (Size[]){RCX, RDX, R8, R9} : (Size[]){RDI, RSI, RDX, RCX, R8, R9};
		Size NInputRegisters = Convention == AMD64_WIN ? 4 : 6;
		for (int i = 0; i < f->ArgumentsCount; i += 1) {
			if (i < NInputRegisters) {
				Size TargetLocation = InputRegisters[i];
				Size CurrentLocation = c->VarsLocation[i];
				if (TargetLocation != CurrentLocation) {
					if (c->CurrentRegsVar[TargetLocation] != 0) {
						SIR_AMD64ForceRegToMem(c, TargetLocation);
					}
					SIR_AMD64WriteMov(c, TargetLocation, CurrentLocation, 0, 0);
				}

				c->VarsLocation[i] = 0;
				c->FreeRegs |= 1 << TargetLocation;
				c->CurrentRegsVar[TargetLocation] = 0;
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

		for (Size i = 0; i < c->NCalleeSavedRegisters; i += 1) {
			SIR_AMD64PushPopReg(c, c->CalleeSavedRegisters[i], 1);
		}
		*f->FunctionPointerToOverride = &c->ExecutableMemory[c->ExecutableMemoryCursor];
	}
}

#undef WriteByte
