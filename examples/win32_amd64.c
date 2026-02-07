#define WIN32_LEAN_AND_MEAN
#include <sir.h>
#include <stdio.h>
#include <windows.h>

#define len(x) (sizeof(x)/sizeof((x)[0]))

int main(void) {
	void *ExecMem = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

	SIR_Function Functions[2] = {0};
	long long (*SumMinus20)(long long, long long);
	Functions[0].FunctionPointerToOverride = (void **)&SumMinus20;
	Functions[0].ArgumentsCount = 2;
	Functions[0].ReturnCount = 1;
	Functions[0].Operations = (SIR_Operation[]){
		 (SIR_Operation){.Instruction = SIR_Add, .InstructionOptions = SIR_Var, .OperandW1 = 0, .OperandW2 = 1},
		 (SIR_Operation){.Instruction = SIR_Sub, .InstructionOptions = SIR_Immediate, .OperandW1 = 2, .OperandDW2 = 20},
		 (SIR_Operation){.Instruction = SIR_Ret, .OperandW1 = 3},
	};
	Functions[0].OperationsCount = 3;

	long long (*SMulTimes10)(long long, long long);
	Functions[1].FunctionPointerToOverride = (void **)&SMulTimes10;
	Functions[1].ArgumentsCount = 2;
	Functions[1].ReturnCount = 1;
	Functions[1].Operations = (SIR_Operation[]){
		 (SIR_Operation){.Instruction = SIR_SMul, .InstructionOptions = SIR_Var, .OperandW1 = 0, .OperandW2 = 1},
		 (SIR_Operation){.Instruction = SIR_SMul, .InstructionOptions = SIR_Immediate, .OperandW1 = 2, .OperandDW2 = 10},
		 (SIR_Operation){.Instruction = SIR_Ret, .OperandW1 = 3},
	};
	Functions[1].OperationsCount = 3;


	SIR_AMD64Compile(Functions, len(Functions), ExecMem, 4096, NULL, 0, NULL, AMD64_WIN);

	long long o;
	o = SumMinus20(30, 40);
	printf("SumMinus20(30, 40) = %lld\n", o);
	o = SumMinus20(100, -50);
	printf("SumMinus20(100, -50) = %lld\n", o);

	o = SMulTimes10(5, 2);
	printf("SMulTimes10(5, 2) = %lld\n", o);
	o = SMulTimes10(6, -4);
	printf("SMulTimes10(6, -4) = %lld\n", o);
	

	return 0;
}
