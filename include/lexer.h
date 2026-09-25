#ifndef FILE_LEXER_H
#define FILE_LEXER_H
enum Token {
  tok_eof = -1,

  tok_def = -2,
  tok_extern = -3,

  tok_identifier = -4,
  tok_number = -5,

  tok_if = -6,
  tok_then = -7,
  tok_else = -8,

  tok_for = -9,
  tok_in = -10,
};

int gettok();
#endif
