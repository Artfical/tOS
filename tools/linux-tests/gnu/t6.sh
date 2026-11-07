#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/system/pkg/gcc/bin:/system/pkg/binutils/bin:/system/pkg/make/bin:/system/pkg/bison/bin:/system/pkg/flex/bin:/system/pkg/m4/bin:/system/pkg/perl/bin:/system/pkg/autoconf/bin:/system/pkg/automake/bin:/system/pkg/libtool/bin:/system/pkg/cmake/bin:/system/pkg/ninja/bin:/system/pkg/pkgconf/bin:/system/pkg/coreutils/bin:/system/pkg/sed/bin:/system/pkg/grep/bin:/system/pkg/gawk/bin:/system/pkg/bash/bin:/system/pkg/busybox/bin:/usr/bin:/bin:$PATH:/usr/bin:/bin
cd /tmp; rm -rf bf; mkdir bf; cd bf
cat > calc.y <<'XX'
%{
#include <stdio.h>
#include <ctype.h>
int yylex(void); void yyerror(const char *s) { fprintf(stderr, "err: %s\n", s); }
%}
%token NUM
%left '+' '-'
%left '*' '/'
%%
input: | input line ;
line: '\n' | expr '\n' { printf("= %d\n", $1); } ;
expr: NUM | expr '+' expr { $$ = $1 + $3; } | expr '-' expr { $$ = $1 - $3; }
    | expr '*' expr { $$ = $1 * $3; } | expr '/' expr { $$ = $1 / $3; } | '(' expr ')' { $$ = $2; } ;
%%
int main(void) { return yyparse(); }
XX
sed -i 's/^int yylex(void);/extern int yylex(void);/' calc.y
cat > calc.l <<'XX'
%{
#include "calc.tab.h"
extern int yylval;
%}
%option noyywrap
%%
[0-9]+  { yylval = atoi(yytext); return NUM; }
[ \t]   ;
\n|.    { return yytext[0]; }
%%
XX
sed -i '1i %{\n#include <stdlib.h>\n%}' calc.l
bison -d calc.y && echo "gt: ok bison -d" || echo "gt: FAIL bison -d"
flex calc.l && echo "gt: ok flex" || echo "gt: FAIL flex"
gcc -O1 -o calc calc.tab.c lex.yy.c 2>&1 | head -5
chk "$(printf '2+3*4\n(1+2)*(3+4)\n100/7-1\n' | ./calc | tr '\n' ' ')" "= 14 = 21 = 13 " "bison+flex calculator built with gcc"
chk "$(flex --version)" "flex 2.6.4" "flex version"
chk "$(bison --version | head -1)" "bison (GNU Bison) 3.8.2" "bison version"
