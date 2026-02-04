#define WIN32_LEAN_AND_MEAN
#include <sir.h>
#include <stdio.h>
#include <windows.h>

int main(void) {
	void *ExecMem = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

	SIR_Function f = {0};
	long long (*SumMinus20)(long long, long long);
	f.FunctionPointerToOverride = (void **)&SumMinus20;
	f.ArgumentsCount = 2;
	f.ReturnCount = 1;
	f.Operations = (SIR_Operation[]){
		 (SIR_Operation){.Instruction = SIR_Add, .InstructionOptions = SIR_Var, .OperandW1 = 0, .OperandW2 = 1},
		 (SIR_Operation){.Instruction = SIR_Sub, .InstructionOptions = SIR_Immediate, .OperandW1 = 2, .OperandDW2 = 20},
		 (SIR_Operation){.Instruction = SIR_Ret, .OperandW1 = 3},
	};
	f.OperationsCount = 3;

	SIR_AMD64Compile(&f, 1, ExecMem, 4096, NULL, 0, NULL, AMD64_WIN);

	puts("Before Call");
	long long a = SumMinus20(30, 40);
	printf("If %lld = 50, it worked!\n", a);

	return 0;
}
