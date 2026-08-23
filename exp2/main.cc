#include <iostream>
#include <FlexLexer.h>
#include "parser.tab.hh"

// parser.y 的 %code 中定义的 lexer 指针
extern yyFlexLexer* lexer;

int main()
{
  std::cout << "Simple Database (C++ Version)" << std::endl;
  std::cout << "Statements end with ';'. Supported syntax:" << std::endl;
  std::cout << "  CREATE TABLE name (col type, ...);   types: int, float, char, double" << std::endl;
  std::cout << "  INSERT INTO name VALUES (value, ...); values: number, 'string', + - * / expr" << std::endl;
  std::cout << "  DROP TABLE name;" << std::endl;
  std::cout << "  SHOW;  QUIT;" << std::endl;
  std::cout << "> " << std::flush;

  static yyFlexLexer flex;
  lexer = &flex;

  yy::location loc;
  yy::parser parser(loc);

  return parser.parse();
}
