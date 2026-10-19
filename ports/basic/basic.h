/* SPDX-License-Identifier: MIT */
#ifndef RIPPLE_BASIC_H
#define RIPPLE_BASIC_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#define BASIC_SOURCE 524288UL
#define BASIC_STRING 4096
#define BASIC_TOKENS 50000
#define BASIC_CODE 16384
#define BASIC_DEPTH 64
#define BASIC_VARS 256
#define BASIC_FILES 16
enum { TK_NAME=256, TK_STRING, TK_NUMBER, TK_LE, TK_GE, TK_NE };
typedef struct { int kind; char *text; double number; unsigned long column; } Token;
enum { OP_ASSIGN, OP_PRINT, OP_WRITE, OP_INPUT, OP_OPEN, OP_CLOSE,
       OP_IF, OP_ELSEIF, OP_ELSE, OP_ENDIF, OP_SELECT, OP_CASE, OP_ENDSELECT,
       OP_GOTO, OP_LABEL, OP_CALL, OP_SUB, OP_RETURN, OP_END, OP_NOP,
       OP_CLS, OP_COLOR, OP_LOCATE, OP_SLEEP, OP_RANDOMIZE,
       OP_FOR, OP_NEXT, OP_WHILE, OP_WEND };
typedef struct { Token *tokens; unsigned count; unsigned long line; unsigned scope; int op, jump, end; } Instruction;
typedef struct { char *name; Token *params; unsigned count; int entry; } Procedure;
typedef struct { double number; char *string; } Value;
typedef struct Variable Variable;
struct Variable { char *name; Value value; Variable *alias, *next; };
typedef struct { int pc; double limit, step; Variable *variable; } Loop;
typedef struct Frame Frame;
struct Frame { Variable *variables; Frame *caller; int return_pc; unsigned scope, count, loops; Loop loop[BASIC_DEPTH]; };
typedef struct { Token *tokens; unsigned count, at; Frame *frame; } Parser;
typedef struct { Instruction *code; unsigned count; Procedure proc[64]; unsigned procedures; } Program;
extern Program basic;
extern Instruction *location;
extern const char *source_path;
extern unsigned long column;
extern uint32_t random_state;
extern int check_only;

void fail(const char *message);
void *allocate(size_t size);
char *copy_text(const char *text);
char *copy_range(const char *text, unsigned n);
int is_word(Token *token, const char *word);
int accept_token(Parser *p, int kind);
int word(Parser *p, const char *name);
void require(Parser *p, int kind);
void require_word(Parser *p, const char *name);
Token *name_token(Parser *p);
void finished(Parser *p);
void load_program(FILE *input);
void free_program(void);
void run_program(void);
Value number_value(double n);
Value string_value(const char *s);
Value clone_value(Value v);
void free_value(Value v);
double numeric(Value v);
long integer_value(Value v);
int truth(Value v);
Variable *variable(Frame *frame, const char *name);
Variable *referent(Variable *v);
void assign(Variable *v, Value value);
void free_frame(Frame *frame);
Value expression(Parser *p, int precedence);
char *number_text(double number, int leading_space);
void output_text(FILE *file, const char *text);
#endif
