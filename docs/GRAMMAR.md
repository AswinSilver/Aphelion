# Aphelion grammar (0.1)

The notation below is EBNF-like.  `IDENTIFIER`, `WHOLE`, `DECIMAL`, `TEXT`,
and `GLYPH` are tokens produced by the lexer.  `NEWLINE` and `;` are statement
separators.  Whitespace and comments are removed before parsing.

## Program and statements

```ebnf
program          ::= separator* (summon-statement separator+)* top-level* EOF ;
top-level        ::= orbit-declaration | declaration | statement ;
summon-statement ::= "summon" TEXT ;

statement        ::= declaration
                   | yield-statement
                   | when-statement
                   | cycle-statement
                   | traverse-statement
                   | escape-statement
                   | skip-statement
                   | block
                   | expression ;

block            ::= "{" separator* [statement
                     (separator+ statement)* separator*] "}" ;

separator        ::= NEWLINE | ";" ;

declaration      ::= ("forge" | "shift" | "anchor")
                     [type-name] IDENTIFIER [":" type-name]
                     "=" expression ;

yield-statement  ::= "yield" [expression] ;
when-statement   ::= "when" expression block
                     ["otherwise" block] ;
cycle-statement  ::= "cycle" expression block ;
traverse-statement ::= "traverse" IDENTIFIER "in" expression block ;
escape-statement ::= "escape" ;
skip-statement   ::= "skip" ;
```

`when` and `cycle` use ordinary expressions, including matched parentheses for
grouping a subexpression or the entire condition. Simple statements need a
newline/semicolon unless followed by a closing brace or EOF. Block-shaped
statements can follow one another within a block; top-level statements still
need a separator. Physical newlines inside strings are errors; a newline after
an operator, assignment, opening parenthesis, or comma continues the expression.

The traverse production was specified before implementation. Its iteration
binding is immutable and loop-local, and its iterable is evaluated once in the
outer scope. Only typed immutable lists are iterable in this first design.
Summon paths use slash-separated ASCII identifier segments without a suffix;
imports are restricted to the initial file preamble. An import at EOF may omit
its final separator, like other simple statements.

## Functions

```ebnf
orbit-declaration ::= "orbit" IDENTIFIER "(" [parameter ("," parameter)*] ")"
                      "-" ">" type-name block ;
parameter        ::= [type-name] IDENTIFIER [":" type-name] ;
type-name        ::= "whole" | "decimal" | "signal" | "text"
                   | "glyph" | "silent" | "[" list-element-type "]" ;
list-element-type ::= "whole" | "decimal" | "signal" | "text" | "glyph" ;
```

Parameters and return types must be explicit after semantic analysis.  The
alternate prefix form (`whole value`) is accepted for the initial release;
the documented canonical form is `value: whole`.

## Expressions

```ebnf
expression       ::= assignment ;
assignment       ::= logical-or [assignment-operator assignment] ;
assignment-operator ::= "=" | "+=" | "-=" | "*=" | "/=" ;
logical-or       ::= logical-and {("or" | "||") logical-and} ;
logical-and      ::= equality {("and" | "&&") equality} ;
equality         ::= comparison {("==" | "!=") comparison} ;
comparison       ::= term {("<" | "<=" | ">" | ">=") term} ;
term             ::= factor {("+" | "-") factor} ;
factor           ::= unary {("*" | "/" | "%") unary} ;
unary            ::= ("!" | "not" | "+" | "-") unary | postfix ;
postfix          ::= primary {"(" [expression ("," expression)*] ")"
                             | "[" expression "]"} ;
primary          ::= WHOLE | DECIMAL | TEXT | GLYPH | "bright" | "dim"
                   | "hollow" | "receive" | "reveal" | IDENTIFIER
                   | "(" expression ")" | list-literal ;
list-literal     ::= "[" [expression ("," expression)* [","]] "]" ;
```

Assignment is right-associative.  All binary operator levels are left-
associative.  Calls bind tighter than unary operators.  There is no implicit
newline continuation after a complete primary when the next token begins a
new statement.

## Lexical tokens

Identifiers use `[A-Za-z_][A-Za-z0-9_]*`.  Whole literals are signed only by
the unary `-` operator and contain decimal digits.  Decimal literals contain
an optional fractional part and/or exponent (`19.95`, `.5`, `1e3`); a decimal
point, if present, must be followed by digits. String literals use double quotes
and glyph literals use single quotes.  Supported escapes are `\n`, `\t`,
`\r`, `\\`, `\"`, `\'`, `\0`, `\a`, `\b`, `\f`, `\v`, and `\xNN`.
The lexer validates escapes and numeric
syntax and records the starting line and column on every token.

The longest operator is selected first: `==`, `!=`, `<=`, `>=`, `+=`, `-=`,
`*=`, `/=`, `&&`, and `||` are single tokens.  `->` is represented by the
two-token sequence `-` `>` so the parser can use the same primitive operator
lexer without inventing an unneeded token kind.
