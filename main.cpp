#include "parser.h"
#include <llvm/IR/Module.h>

#include <map>

extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::IRBuilder<>> Builder;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::map<std::string, llvm::Value *> NamedValues;

int main(int argc, char *argv[]) {
  fprintf(stderr, "ready> ");
  getNextToken();

  InitializeModule();

  MainLoop();

  TheModule->print(llvm::errs(), nullptr);

  return 0;
}
