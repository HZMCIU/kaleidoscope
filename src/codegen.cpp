#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include <cassert>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassInstrumentation.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/Error.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <vector>

#include "KaleidoscopeJIT.h"
#include "parser.h"

std::unique_ptr<llvm::LLVMContext> TheContext;
std::unique_ptr<llvm::IRBuilder<>> Builder;
std::unique_ptr<llvm::Module> TheModule;
std::map<std::string, llvm::AllocaInst *> NamedValues;
std::unique_ptr<llvm::FunctionPassManager> TheFPM;
std::unique_ptr<llvm::LoopAnalysisManager> TheLAM;
std::unique_ptr<llvm::FunctionAnalysisManager> TheFAM;
std::unique_ptr<llvm::CGSCCAnalysisManager> TheCGAM;
std::unique_ptr<llvm::ModuleAnalysisManager> TheMAM;
std::unique_ptr<llvm::PassInstrumentationCallbacks> ThePIC;
std::unique_ptr<llvm::StandardInstrumentations> TheSI;
std::unique_ptr<llvm::orc::KaleidoscopeJIT> TheJIT;
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

extern std::map<char, int> BinopPrecedence;

llvm::Function *getFunction(std::string Name);

DebugInfo KSDbgInfo;
std::unique_ptr<llvm::DIBuilder> DBuilder;

SourceLocation CurLoc;
SourceLocation LexLoc = {1, 0};

llvm::DIType *DebugInfo::getDoubleTy() {
  if (DblTy) {
    return DblTy;
  }

  DblTy = DBuilder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);

  return DblTy;
}

void DebugInfo::emitLocation(ExprAST *AST) {
  if (!AST) {
    return Builder->SetCurrentDebugLocation(llvm::DebugLoc());
  }

  llvm::DIScope *Scope;
  if (LexicalBlocks.empty()) {
    Scope = TheCU;
  } else {
    Scope = LexicalBlocks.back();
  }

  Builder->SetCurrentDebugLocation(llvm::DILocation::get(
      Scope->getContext(), AST->getLine(), AST->getCol(), Scope));
}

static int advance() {
  int LastChar = getchar();

  if (LastChar == '\n' || LastChar == '\r') {
    LexLoc.Line++;
    LexLoc.Col = 0;
  } else {
    LexLoc.Col++;
  }

  return LastChar;
}

static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *TheFunction,
                                                llvm::StringRef VarName) {
  llvm::IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                         TheFunction->getEntryBlock().begin());

  return TmpB.CreateAlloca(llvm::Type::getDoubleTy(*TheContext), nullptr,
                           VarName);
}

llvm::Value *NumberExprAST::codegen() {
  KSDbgInfo.emitLocation(this);
  return llvm::ConstantFP::get(*TheContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen() {
  // llvm::Value *V = NamedValues[Name];
  llvm::AllocaInst *A = NamedValues[Name];

  if (!A) {
    LogErrorV("Unknown variable name");
  }

  KSDbgInfo.emitLocation(this);

  return Builder->CreateLoad(A->getAllocatedType(), A, Name.c_str());
}

llvm::Value *BinaryExprAST::codegen() {
  KSDbgInfo.emitLocation(this);

  llvm::Value *L = LHS->codegen();
  llvm::Value *R = RHS->codegen();

  if (!L || !R) {
    return nullptr;
  }

  switch (Op) {
  case '+':
    return Builder->CreateFAdd(L, R, "addtmp");
  case '-':
    return Builder->CreateFSub(L, R, "subtmp");
  case '*':
    return Builder->CreateFMul(L, R, "multmp");
  case '<':
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    return Builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*TheContext),
                                 "booltmp");
  case '=':
    llvm::Value *Val = RHS->codegen();
    if (!Val) {
      return nullptr;
    }

    if (Op == '=') {
      VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
      if (!LHSE) {
        return LogErrorV("definition of '=' must be a variable");
      }

      llvm::Value *Val = RHS->codegen();
      if (!Val) {
        return nullptr;
      }

      llvm::AllocaInst *Alloca = NamedValues[LHSE->getName()];
      if (!Alloca) {
        return LogErrorV("Unknown variable name");
      }
      Builder->CreateStore(Val, Alloca);
    }

    return Val;
  }

  llvm::Function *F = getFunction(std::string("binary") + Op);
  assert(F && "binary operator not found!");

  llvm::Value *Ops[2] = {L, R};
  return Builder->CreateCall(F, Ops, "binop");
}

llvm::Function *getFunction(std::string Name) {
  if (auto *F = TheModule->getFunction(Name)) {
    return F;
  }

  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end()) {
    return FI->second->codegen();
  }

  return nullptr;
}

llvm::Value *CallExprAST::codegen() {
  KSDbgInfo.emitLocation(this);
  // llvm::Function *CalleeF = TheModule->getFunction(Callee);
  llvm::Function *CalleeF = getFunction(Callee);
  if (!CalleeF) {
    return LogErrorV("Unknown function referenced");
  }

  if (CalleeF->arg_size() != Args.size()) {
    return LogErrorV("Incorrect # arguments passed");
  }

  std::vector<llvm::Value *> ArgsV;

  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    ArgsV.push_back(Args[i]->codegen());
    if (!ArgsV.back()) {
      return nullptr;
    }
  }

  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

llvm::Function *PrototypeAST::codegen() {
  std::vector<llvm::Type *> Doubles(Args.size(),
                                    llvm::Type::getDoubleTy(*TheContext));

  llvm::FunctionType *FT = llvm::FunctionType::get(
      llvm::Type::getDoubleTy(*TheContext), Doubles, false);

  llvm::Function *F = llvm::Function::Create(
      FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

  unsigned Idx = 0;
  for (auto &Arg : F->args()) {
    Arg.setName(Args[Idx++]);
  }

  return F;
}

static llvm::DISubroutineType *CreateFunctionType(unsigned NumArgs) {
  llvm::SmallVector<llvm::Metadata *, 8> EltTyps;
  llvm::DIType *DblTy = KSDbgInfo.getDoubleTy();

  EltTyps.push_back(DblTy);

  for (unsigned i = 0, e = NumArgs; i != e; i++) {
    EltTyps.push_back(DblTy);
  }

  return DBuilder->createSubroutineType(
      DBuilder->getOrCreateTypeArray(EltTyps));
}

llvm::Function *FunctionAST::codegen() {
  auto &P = *Proto;
  FunctionProtos[Proto->getName()] = std::move(Proto);
  llvm::Function *TheFunction = getFunction(P.getName());

  if (!TheFunction) {
    return nullptr;
  }

  if (P.isBinaryOp()) {
    BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();
  }

  llvm::BasicBlock *BB =
      llvm::BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  llvm::DIFile *Unit = DBuilder->createFile(KSDbgInfo.TheCU->getFilename(),
                                            KSDbgInfo.TheCU->getDirectory());
  llvm::DIScope *FContext = Unit;
  unsigned LineNo = 0;
  unsigned ScopeLine = 0;
  llvm::DISubprogram *SP = DBuilder->createFunction(
      FContext, P.getName(), llvm::StringRef(), Unit, LineNo,
      CreateFunctionType(TheFunction->arg_size()), ScopeLine,
      llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
  TheFunction->setSubprogram(SP);
  KSDbgInfo.LexicalBlocks.push_back(SP);
  KSDbgInfo.emitLocation(nullptr);

  NamedValues.clear();
  int ArgIdx = 0;
  for (auto &Arg : TheFunction->args()) {
    // NamedValues[std::string(Arg.getName())] = &Arg;
    llvm::AllocaInst *Alloca =
        CreateEntryBlockAlloca(TheFunction, Arg.getName());

    llvm::DILocalVariable *D = DBuilder->createParameterVariable(
        SP, Arg.getName(), ++ArgIdx, Unit, LineNo, KSDbgInfo.getDoubleTy());

    DBuilder->insertDeclare(
        Alloca, D, DBuilder->createExpression(),
        llvm::DILocation::get(SP->getContext(), LineNo, 0, SP),
        Builder->GetInsertBlock());

    Builder->CreateStore(&Arg, Alloca);
    NamedValues[std::string(Arg.getName())] = Alloca;
  }

  KSDbgInfo.emitLocation(Body.get());

  if (llvm::Value *RetVal = Body->codegen()) {
    Builder->CreateRet(RetVal);

    KSDbgInfo.LexicalBlocks.pop_back();

    llvm::verifyFunction(*TheFunction);

    // TheFPM->run(*TheFunction, *TheFAM);

    return TheFunction;
  }

  TheFunction->eraseFromParent();

  if (P.isBinaryOp()) {
    BinopPrecedence.erase(P.getOperatorName());
  }

  KSDbgInfo.LexicalBlocks.pop_back();

  return nullptr;
}

llvm::Value *IfExprAST::codegen() {
  KSDbgInfo.emitLocation(this);
  llvm::Value *ConV = Cond->codegen();
  if (!ConV) {
    return nullptr;
  }

  ConV = Builder->CreateFCmpONE(
      ConV, llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0)), "ifcond");

  llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();
  llvm::BasicBlock *ThenBB =
      llvm::BasicBlock::Create(*TheContext, "then", TheFunction);
  llvm::BasicBlock *ElseBB = llvm::BasicBlock::Create(*TheContext, "else");
  llvm::BasicBlock *MergeBB = llvm::BasicBlock::Create(*TheContext, "ifcont");

  Builder->CreateCondBr(ConV, ThenBB, ElseBB);

  Builder->SetInsertPoint(ThenBB);
  llvm::Value *ThenV = Then->codegen();
  if (!ThenV) {
    return nullptr;
  }
  Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  llvm::Value *ElseV = Else->codegen();
  if (!ElseV) {
    return nullptr;
  }
  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  llvm::PHINode *PN =
      Builder->CreatePHI(llvm::Type::getDoubleTy(*TheContext), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);

  return PN;
}

llvm::Value *ForExprAST::codegen() {

  llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

  llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);

  KSDbgInfo.emitLocation(this);

  llvm::Value *StartVal = Start->codegen();
  if (!StartVal) {
    return nullptr;
  }
  Builder->CreateStore(StartVal, Alloca);
  // llvm::BasicBlock *PreheaderBB = Builder->GetInsertBlock();
  llvm::BasicBlock *LoopBB =
      llvm::BasicBlock::Create(*TheContext, "loop", TheFunction);

  Builder->CreateBr(LoopBB);

  Builder->SetInsertPoint(LoopBB);

  // llvm::PHINode *Variable =
  //     Builder->CreatePHI(llvm::Type::getDoubleTy(*TheContext), 2, VarName);
  // Variable->addIncoming(StartVal, PreheaderBB);

  llvm::AllocaInst *OldVal = NamedValues[VarName];
  NamedValues[VarName] = Alloca;
  // NamedValues[VarName] = Variable;

  if (!Body->codegen()) {
    return nullptr;
  }

  llvm::Value *StepVal = nullptr;
  if (Step) {
    StepVal = Step->codegen();
    if (!StepVal) {
      return nullptr;
    }
  } else {
    StepVal = llvm::ConstantFP::get(*TheContext, llvm::APFloat(1.0));
  }

  llvm::Value *EndCond = End->codegen();
  if (!EndCond) {
    return nullptr;
  }

  llvm::Value *CurVal =
      Builder->CreateLoad(Alloca->getAllocatedType(), Alloca, VarName.c_str());
  llvm::Value *NextVar = Builder->CreateFAdd(CurVal, StepVal, "nextvar");
  Builder->CreateStore(NextVar, Alloca);
  // llvm::Value *NextVar = Builder->CreateFAdd(Variable, StepVal, "nextvar");

  EndCond = Builder->CreateFCmpONE(
      EndCond, llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0)),
      "loopcond");

  // llvm::BasicBlock *LoopEndBB = Builder->GetInsertBlock();
  llvm::BasicBlock *AfterBB =
      llvm::BasicBlock::Create(*TheContext, "afterloop", TheFunction);
  Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
  Builder->SetInsertPoint(AfterBB);

  // Variable->addIncoming(NextVar, LoopEndBB);

  if (OldVal) {
    NamedValues[VarName] = OldVal;
  } else {
    NamedValues.erase(VarName);
  }

  return llvm::Constant::getNullValue(llvm::Type::getDoubleTy(*TheContext));
}

llvm::Value *UnaryExprAST::codegen() {
  llvm::Value *OperandV = Operand->codegen();
  if (!OperandV) {
    return nullptr;
  }

  llvm::Function *F = getFunction(std::string("unary") + Opcode);
  if (!F) {
    return LogErrorV("Unknow unary operator");
  }

  KSDbgInfo.emitLocation(this);
  return Builder->CreateCall(F, OperandV, "unop");
}

llvm::Value *VarExprAST::codegen() {
  std::vector<llvm::AllocaInst *> OldBindings;

  llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

  for (unsigned i = 0, e = VarNames.size(); i != e; i++) {
    const std::string &VarName = VarNames[i].first;
    ExprAST *Init = VarNames[i].second.get();

    llvm::Value *InitVal;
    if (Init) {
      InitVal = Init->codegen();
      if (!InitVal) {
        return nullptr;
      }
    } else {
      InitVal = llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0));
    }

    llvm::AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
    Builder->CreateStore(InitVal, Alloca);

    OldBindings.push_back(NamedValues[VarName]);

    NamedValues[VarName] = Alloca;
  }

  KSDbgInfo.emitLocation(this);

  llvm::Value *BodyVal = Body->codegen();
  if (!BodyVal) {
    return nullptr;
  }

  for (unsigned i = 0, e = VarNames.size(); i != e; i++) {
    NamedValues[VarNames[i].first] = OldBindings[i];
  }
  return BodyVal;
}

void InitializeModule() {
  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule = std::make_unique<llvm::Module>("my cool jit", *TheContext);
  Builder = std::make_unique<llvm::IRBuilder<>>(*TheContext);
}

void InitializeModuleAndManagers() {
  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule = std::make_unique<llvm::Module>("KaleidoscopeJIT", *TheContext);
  TheModule->setDataLayout(TheJIT->getDataLayout());

  Builder = std::make_unique<llvm::IRBuilder<>>(*TheContext);

  TheFPM = std::make_unique<llvm::FunctionPassManager>();
  TheLAM = std::make_unique<llvm::LoopAnalysisManager>();
  TheFAM = std::make_unique<llvm::FunctionAnalysisManager>();
  TheCGAM = std::make_unique<llvm::CGSCCAnalysisManager>();
  TheMAM = std::make_unique<llvm::ModuleAnalysisManager>();
  ThePIC = std::make_unique<llvm::PassInstrumentationCallbacks>();
  TheSI = std::make_unique<llvm::StandardInstrumentations>(*TheContext, true);

  TheSI->registerCallbacks(*ThePIC, TheMAM.get());

  TheFPM->addPass(llvm::PromotePass());
  TheFPM->addPass(llvm::InstCombinePass());
  TheFPM->addPass(llvm::ReassociatePass());
  TheFPM->addPass(llvm::GVNPass());
  TheFPM->addPass(llvm::SimplifyCFGPass());

  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(*TheMAM);
  PB.registerFunctionAnalyses(*TheFAM);
  PB.crossRegisterProxies(*TheLAM, *TheFAM, *TheCGAM, *TheMAM);
}
