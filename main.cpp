#include "lexer.h"
#include "parser.h"

#include <map>

int main(int argc, char *argv[]) {
  fprintf(stderr, "ready> ");
  getNextToken();

  MainLoop();
  return 0;
}
