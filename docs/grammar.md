# Tether Grammar

Informal EBNF-style grammar for the v0.1 parser. This is the grammar
the parser actually accepts; it is not yet a normative specification.

## Lexical structure

```
comment        := '//' until-newline
               | '/*' until-'*/'

ident          := ident_start ident_continue*
ident_start    := 'a'..'z' | 'A'..'Z' | '_'
ident_continue := ident_start | '0'..'9'

int_literal    := digit (('_' | digit)* digit)?
               | '0x' hex_digit+
               | '0b' bin_digit+
               | '0o' oct_digit+

float_literal  := digit+ '.' digit+ (exponent)?
               | digit+ exponent
exponent       := ('e' | 'E') ('+' | '-')? digit+

string_literal := '"' (string_char | string_escape)* '"'
string_char    := any except '"', '\\', control
string_escape  := '\\' ('n' | 't' | 'r' | '0' | '"' | '\\' | 'x' hex hex)

char_literal   := "'" (string_char | string_escape) "'"

bool_literal   := 'true' | 'false'
```

### Keywords

```
module import export
fn struct enum union trait impl type alias
let mut const static
if else match while for loop break continue return defer
alloc move borrow unsafe
extern ffi comptime
spawn await
```

`self`, `true`, `false` are contextual.

### Operators and punctuation

```
+  -  *  /  %
== != <  >  <= >=
&& || !
=  += -= *= /= %=   <- assignment
?  :  .  ,  ;
(  )  {  }  [  ]
-> =>  ::  #  @  &  |  ^  ~  <<  >>
```

## Syntax

### Module

```
module_decl    := 'module' module_path
module_path    := ident ('::' ident)*

import_decl    := 'import' module_path ('as' ident)?
export_decl    := 'export' decl
```

### Declarations

```
decl           := module_decl
               | import_decl
               | export_decl
               | fn_decl
               | struct_decl
               | enum_decl
               | union_decl
               | trait_decl
               | impl_decl
               | type_decl
               | alias_decl
               | const_decl
               | static_decl
               | extern_decl

fn_decl        := 'fn' ident type_params? '(' params? ')' ('->' type)? block
type_params    := '<' type_param (',' type_param)* '>'
type_param     := ident (':' trait_ref (',' trait_ref)*)?
params         := param (',' param)*
param          := (self_param | ident) ':' type
self_param     := 'self' | 'borrow' 'self' | 'borrow' 'mut' 'self'
block          := '{' stmt* '}'

struct_decl    := 'struct' ident type_params? '{' field (',' field)* '}'
field          := ident ':' type

enum_decl      := 'enum' ident type_params? '{' variant (',' variant)* '}'
variant        := ident ('(' type_list ')')?

union_decl     := 'union' ident type_params? '{' field (',' field)* '}'

trait_decl     := 'trait' ident type_params? '{' trait_member* '}'
trait_member   := fn_signature
fn_signature   := 'fn' ident type_params? '(' params? ')' ('->' type)?

impl_decl      := 'impl' type_params? type ('for' type)? '{' impl_member* '}'
impl_member    := fn_decl | const_decl

type_decl      := 'type' ident type_params? '=' type
alias_decl     := 'alias' ident '=' type

const_decl     := 'const' ident (':' type)? '=' expr
static_decl    := 'static' ident (':' type)? '=' expr

extern_decl    := 'extern' 'fn' ident '(' extern_params? ')' ('->' type)?
extern_params  := extern_param (',' extern_param)*
extern_param   := ident ':' type | '...'
```

### Statements

```
stmt           := let_stmt
               | expr_stmt
               | return_stmt
               | defer_stmt
               | break_stmt
               | continue_stmt
               | unsafe_block
               | block

let_stmt       := 'let' 'mut'? ident (':' type)? '=' expr
expr_stmt      := expr
return_stmt    := 'return' expr?
defer_stmt     := 'defer' expr
break_stmt     := 'break'
continue_stmt  := 'continue'
unsafe_block   := 'unsafe' block
```

### Expressions (Pratt-parsed)

Precedence from lowest to highest:

```
expr           := assignment_expr
assignment_expr:= or_expr (assign_op or_expr)?
assign_op      := '=' | '+=' | '-=' | '*=' | '/=' | '%='
or_expr        := and_expr ('||' and_expr)*
and_expr       := eq_expr ('&&' eq_expr)*
eq_expr        := cmp_expr (('==' | '!=') cmp_expr)*
cmp_expr       := bit_or_expr (('<' | '>' | '<=' | '>=') bit_or_expr)*
bit_or_expr    := bit_xor_expr ('|' bit_xor_expr)*
bit_xor_expr   := bit_and_expr ('^' bit_and_expr)*
bit_and_expr   := shift_expr ('&' shift_expr)*
shift_expr     := add_expr (('<<' | '>>') add_expr)*
add_expr       := mul_expr (('+' | '-') mul_expr)*
mul_expr       := unary_expr (('*' | '/' | '%') unary_expr)*
unary_expr     := ('-' | '!' | 'borrow' 'mut'? | 'move' | '*') unary_expr
               | postfix_expr
postfix_expr   := primary (postfix)*
postfix        := '.' ident            # field access
               | '(' arg_list? ')'     # call
               | '[' expr ']'          # index
               | '::' ident            # path
               | '?'                   # error propagation
primary        := literal
               | ident_or_path
               | '(' expr ')'
               | block_expr
               | if_expr
               | match_expr
               | loop_expr
               | while_expr
               | for_expr
               | spawn_expr
               | alloc_expr
               | borrow_expr
               | move_expr

literal        := int_literal
               | float_literal
               | string_literal
               | char_literal
               | bool_literal

block_expr     := '{' stmt* expr? '}'
if_expr        := 'if' expr block ('else' (if_expr | block))?
match_expr     := 'match' expr '{' match_arm (',' match_arm)* '}'
match_arm      := pattern '=>' expr
loop_expr      := 'loop' block
while_expr     := 'while' expr block
for_expr       := 'for' ident 'in' expr block
spawn_expr     := 'spawn' block
alloc_expr     := 'alloc' alloc_target expr
alloc_target   := 'arena' | 'heap' | ident
borrow_expr    := 'borrow' 'mut'? expr
move_expr      := 'move' expr
```

### Patterns

```
pattern        := '_'
               | ident
               | bool_literal
               | int_literal
               | '-' int_literal
               | string_literal
               | variant_pattern
               | tuple_pattern
               | struct_pattern
               | pattern 'as' ident        # binding
variant_pattern:= path '(' pattern_list? ')'
tuple_pattern  := '(' pattern_list? ')'
struct_pattern := '{' field_pattern (',' field_pattern)* '}'
field_pattern  := ident ':' pattern | ident
pattern_list   := pattern (',' pattern)*
```

### Types

```
type           := ref_type
ref_type       := 'ref' ('(' region ')')? type
               | 'mut' 'ref' ('(' region ')')? type
               | 'borrow' 'mut'? 'ref' type
               | primary_type
primary_type   := path
               | '[' type ';' int_literal ']'
               | '(' type_list? ')'
               | '*' 'const' type           # only in extern
               | '*' 'mut' type             # only in extern
type_list      := type (',' type)*
path           := ident ('::' ident)* type_args?
type_args      := '<' type (',' type)* '>'
region         := ident
trait_ref      := path
```

## Open issues

The following are intentionally unresolved in v0.1:

- Exact precedence of `?` relative to postfix `.` and `()`.
- Whether `borrow`/`move` are prefix unary operators or statement-like
  forms (parser currently treats them as prefix unary).
- Grammar of `rewrite` rules — defined semantically in
  `docs/design.md`, not yet in the formal grammar.
- `comptime` block grammar — `comptime block` for now.
