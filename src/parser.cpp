#include "parser.h"
#include "KaleidoscopeJIT.h"
#include "lexer.h"

#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <map>
#include <memory>

extern double NumVal;
extern std::string IdentifierStr;
extern std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
extern std::unique_ptr<llvm::LLVMContext> TheContext;
extern std::unique_ptr<llvm::Module> TheModule;
extern std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
llvm::ExitOnError ExitOnErr;

int CurTok;
int getNextToken() { return CurTok = gettok(); }

static std::unique_ptr<ExprAST> ParseIfExpr();
static std::unique_ptr<ExprAST> ParseExpression();

static std::unique_ptr<ExprAST> LogError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

static std::unique_ptr<PrototypeAST> LogErrorP(const char *Str) {
  LogError(Str);
  return nullptr;
}

llvm::Value *LogErrorV(const char *Str) {
  LogError(Str);
  return nullptr;
}

static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken();
  return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken();
  auto V = ParseExpression();
  if (!V) {
    return nullptr;
  }

  if (CurTok != ')') {
    return LogError("expected ')");
  }
  getNextToken();

  return V;
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;

  getNextToken();

  if (CurTok != '(') {
    return std::make_unique<VariableExprAST>(IdName);
  }

  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression()) {
        Args.push_back(std::move(Arg));
      } else {
        return nullptr;
      }

      if (CurTok == ')') {
        break;
      }

      if (CurTok != ',') {
        return LogError("Expected ')' or ',' in argument list");
      }

      getNextToken();
    }
  }

  getNextToken();

  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default:
    printf("%c\n", CurTok);
    return LogError("unknown token when expecting an expression");
  case tok_identifier:
    return ParseIdentifierExpr();
  case tok_number:
    return ParseNumberExpr();
  case '(':
    return ParseParenExpr();
  case tok_if:
    return ParseIfExpr();
  }
}

std::map<char, int> BinopPrecedence = {
    {'<', 10},
    {'+', 20},
    {'-', 30},
    {'*', 40},
};

static int GetTokPrecedence() {
  if (!isascii(CurTok)) {
    return -1;
  }

  int TokPrec = BinopPrecedence[CurTok];
  if (TokPrec <= 0) {
    return -1;
  }
  return TokPrec;
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                              std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();
    if (TokPrec < ExprPrec) {
      return LHS;
    }

    int BinOp = CurTok;
    getNextToken();

    auto RHS = ParsePrimary();
    if (!RHS) {
      return nullptr;
    }

    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS) {
        return nullptr;
      }
    }

    LHS =
        std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
  }
}

static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS) {
    return nullptr;
  }
  return ParseBinOpRHS(0, std::move(LHS));
}

static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier) {
    return LogErrorP("Expected function name in prototype");
  }

  std::string FnName = IdentifierStr;
  getNextToken();

  if (CurTok != '(') {
    return LogErrorP("Expected '(' in prototype");
  }

  std::vector<std::string> ArgNames;

  while (getNextToken() == tok_identifier) {
    ArgNames.push_back(IdentifierStr);
  }

  if (CurTok != ')') {
    return LogErrorP("Expected ')' in prototype");
  }

  getNextToken();

  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken();
  auto Proto = ParsePrototype();
  if (!Proto) {
    return nullptr;
  }

  if (auto E = ParseExpression()) {
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }

  return nullptr;
}

static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();
  return ParsePrototype();
}

static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    auto Proto = std::make_unique<PrototypeAST>("__anon_expr",
                                                std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }

  return nullptr;
}

static std::unique_ptr<ExprAST> ParseIfExpr() {
  getNextToken();
  auto Cond = ParseExpression();
  if (!Cond) {
    return nullptr;
  }

  if (CurTok != tok_then) {
    return LogError("expected then");
  }
  getNextToken();
  auto Then = ParseExpression();
  if (!Then) {
    return nullptr;
  }

  if (CurTok != tok_else) {
    return LogError("expected else");
  }
  getNextToken();
  auto Else = ParseExpression();
  if (!Else) {
    return nullptr;
  }

  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                     std::move(Else));
}

static void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
    if (auto *FnIR = FnAST->codegen()) {
      fprintf(stderr, "Read function definition: \n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      ExitOnErr(TheJIT->addModule(llvm::orc::ThreadSafeModule(
          std::move(TheModule), std::move(TheContext))));
      InitializeModuleAndManagers();
    } else {
      getNextToken();
    }
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if (auto *FnIR = ProtoAST->codegen()) {
      fprintf(stderr, "Read extern: \n");
      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
    } else {
      getNextToken();
    }
  }
}

static void HandleTopLevelExpression() {
  if (auto FnAST = ParseTopLevelExpr()) {
    // if (auto *FnIR = FnAST->codegen()) {
    //   fprintf(stderr, "Read top-level expression: \n");
    //   FnIR->print(llvm::errs());
    //   fprintf(stderr, "\n");
    //   FnIR->eraseFromParent();
    if (auto *FnIR = FnAST->codegen()) {

      FnIR->print(llvm::errs());
      fprintf(stderr, "\n");

      auto RT = TheJIT->getMainJITDylib().createResourceTracker();

      auto TSM = llvm::orc::ThreadSafeModule(std::move(TheModule),
                                             std::move(TheContext));

      ExitOnErr(TheJIT->addModule(std::move(TSM), RT));
      InitializeModuleAndManagers();

      auto ExprSymbol = ExitOnErr(TheJIT->lookup("__anon_expr"));

      double (*FP)() = ExprSymbol.toPtr<double (*)()>();
      fprintf(stderr, "Evaluated to %f\n", FP());

      ExitOnErr(RT->remove());
    } else {
      getNextToken();
    }
  }
}

void MainLoop() {
  while (true) {
    fprintf(stderr, "ready> ");
    switch (CurTok) {
    case tok_eof:
      return;
    case ';':
      getNextToken();
      break;
    case tok_def:
      HandleDefinition();
      break;
    case tok_extern:
      HandleExtern();
      break;
    default:
      HandleTopLevelExpression();
      break;
    }
  }
}
