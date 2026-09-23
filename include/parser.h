#include <memory>
#include <string>
#include <vector>

class ExprAST {
public:
  virtual ~ExprAST() = default;
};

class NumberExprAST : public ExprAST {
public:
  NumberExprAST(double Val) : Val(Val) {};

private:
  double Val;
};

class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(const std::string &Name) : Name(Name) {}
};

class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
};

class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(std::string &Callee, std::vector<std::unique_ptr<ExprAST>> Args)
      : Callee(Callee), Args(std::move(Args)) {}
};

class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;

public:
  PrototypeAST(const std::string &Name, std::vector<std::string> Args)
      : Name(Name), Args(std::move(Args)) {}

  const std::string &getName() const { return Name; }
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}
};

// std::unique_ptr<ExprAST> LogError(const char *Str);
// std::unique_ptr<PrototypeAST> LogErrorP(const char *Str);
// std::unique_ptr<ExprAST> ParseNumberExpr();
// std::unique_ptr<ExprAST> ParseParenExpr();
// std::unique_ptr<ExprAST> ParseIdentifierExpr();
// std::unique_ptr<ExprAST> ParsePrimary();
void MainLoop();
int getNextToken();
