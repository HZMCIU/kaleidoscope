#include "KaleidoscopeJIT.h"
#include "parser.h"
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>

#include <map>

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::map<std::string, llvm::Value *> NamedValues;
extern std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
extern llvm::ExitOnError ExitOnErr;

int main(int argc, char *argv[]) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  fprintf(stderr, "ready> ");
  getNextToken();

  TheJIT = ExitOnErr(llvm::orc::KaleidoscopeJIT::Create());
  InitializeModuleAndManagers();

  MainLoop();

  TheModule->print(llvm::errs(), nullptr);

  return 0;
}
