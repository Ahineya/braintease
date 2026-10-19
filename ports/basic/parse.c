/* SPDX-License-Identifier: MIT */
#include "basic.h"

int is_word(Token *t, const char *s) { return t->kind==TK_NAME && !strcmp(t->text,s); }
int accept_token(Parser *p, int kind) {
    if (p->at<p->count) {
        column=p->tokens[p->at].column;
        if (p->tokens[p->at].kind==kind) { ++p->at; return 1; }
    }
    return 0;
}
int word(Parser *p, const char *s) {
    if (p->at<p->count && is_word(&p->tokens[p->at],s)) { column=p->tokens[p->at++].column; return 1; }
    return 0;
}
void require(Parser *p, int kind) { if (!accept_token(p,kind)) fail("unexpected token"); }
void require_word(Parser *p, const char *s) { if (!word(p,s)) fail(s); }
Token *name_token(Parser *p) {
    Token *t;
    if (p->at>=p->count) fail("expected variable name");
    t=&p->tokens[p->at]; column=t->column;
    require(p,TK_NAME); return t;
}
void finished(Parser *p) { if (p->at!=p->count) { column=p->tokens[p->at].column; fail("unexpected token after statement"); } }

typedef struct { int start, last, saw_else; } Block;
static Block blocks[BASIC_DEPTH];
static unsigned depth, scope, total_tokens, inline_depth;
static unsigned long source_size;
static Instruction diagnostic;
static int source_char(FILE *input) {
    int c=fgetc(input);
    if (c!=EOF && ++source_size>BASIC_SOURCE) fail("source exceeds 512 KiB");
    if (c!=EOF && (c==0 || c>127)) fail("source must be ASCII text");
    return c;
}

static void emit(Token *tokens, unsigned n, unsigned long line, int label) {
    Instruction *in;
    Token *t;
    unsigned i;
    int op=-1;
    if (!n) return;
    if (basic.count==BASIC_CODE) fail("program exceeds 16384 statements");
    in=&basic.code[basic.count++];
    in->tokens=allocate(n*sizeof(Token)); memcpy(in->tokens,tokens,n*sizeof(Token));
    in->count=n; in->line=line; in->scope=scope; in->jump=in->end=-1;
    location=in; column=tokens[0].column; t=in->tokens;
    if (label) op=OP_LABEL;
    else if (is_word(t,"IF")) op=OP_IF;
    else if (is_word(t,"ELSEIF")) op=OP_ELSEIF;
    else if (is_word(t,"ELSE")) op=OP_ELSE;
    else if (is_word(t,"SELECT")) op=OP_SELECT;
    else if (is_word(t,"CASE")) op=OP_CASE;
    else if (is_word(t,"FOR")) op=OP_FOR;
    else if (is_word(t,"NEXT")) op=OP_NEXT;
    else if (is_word(t,"WHILE")) op=OP_WHILE;
    else if (is_word(t,"WEND")) op=OP_WEND;
    else if (is_word(t,"END")) {
        op=n==1 ? OP_END : is_word(t+1,"IF") ? OP_ENDIF :
            is_word(t+1,"SELECT") ? OP_ENDSELECT : is_word(t+1,"SUB") ? OP_RETURN : -1;
    } else if (is_word(t,"STOP")) op=OP_END;
    else if (is_word(t,"DECLARE")) op=OP_NOP;
    else if (is_word(t,"REM")) op=OP_NOP;
    else if (is_word(t,"SUB")) op=OP_SUB;
    else if (is_word(t,"CALL")) op=OP_CALL;
    else if (is_word(t,"GOTO")) op=OP_GOTO;
    else if (is_word(t,"PRINT") || t->kind=='?') op=OP_PRINT;
    else if (is_word(t,"WRITE")) op=OP_WRITE;
    else if (is_word(t,"INPUT")) op=OP_INPUT;
    else if (is_word(t,"OPEN")) op=OP_OPEN;
    else if (is_word(t,"CLOSE")) op=OP_CLOSE;
    else if (is_word(t,"CLS")) op=OP_CLS;
    else if (is_word(t,"COLOR")) op=OP_COLOR;
    else if (is_word(t,"LOCATE")) op=OP_LOCATE;
    else if (is_word(t,"SLEEP")) op=OP_SLEEP;
    else if (is_word(t,"RANDOMIZE")) op=OP_RANDOMIZE;
    else if (is_word(t,"LET") || (n>=2 && t->kind==TK_NAME && t[1].kind=='=')) op=OP_ASSIGN;
    if (op<0) fail("unsupported BASIC statement");
    in->op=op;
    if (op==OP_SUB) {
        Procedure *proc;
        Parser p;
        if (scope || depth || basic.procedures==64) fail("invalid or excessive SUB definitions");
        p.tokens=t; p.count=n; p.at=1; p.frame=NULL;
        t=name_token(&p);
        for (i=1;i<basic.procedures;++i) if (!strcmp(basic.proc[i].name,t->text)) fail("duplicate SUB");
        scope=basic.procedures++;
        proc=&basic.proc[scope]; proc->name=t->text; proc->entry=(int)basic.count;
        proc->params=allocate(32*sizeof(Token));
        if (accept_token(&p,'(') && !accept_token(&p,')')) {
            do {
                if (proc->count==32) fail("SUB exceeds 32 parameters");
                t=name_token(&p);
                for (i=0;i<proc->count;++i) if (!strcmp(proc->params[i].text,t->text)) fail("duplicate parameter");
                proc->params[proc->count++]=*t;
            } while (accept_token(&p,','));
            require(&p,')');
        }
        finished(&p);
    } else if (op==OP_RETURN) {
        if (!scope || depth) fail("END SUB without SUB, or unclosed block");
        scope=0;
    }
    /* Inline IF is flattened into a normal IF, body, END IF sequence. */
    if (op==OP_IF) {
        for (i=1;i<n;++i) if (is_word(&in->tokens[i],"THEN")) break;
        if (i==n) fail("IF requires THEN");
        if (i+1<n) {
            unsigned rest=n-i-1;
            Token *body=allocate(rest*sizeof(Token));
            memcpy(body,in->tokens+i+1,rest*sizeof(Token));
            in->count=i+1;
            ++inline_depth;
            emit(body,rest,line,0); free(body);
        }
    }
}

static void close_inline(unsigned long line) {
    while (inline_depth) {
        Token close[2];
        --inline_depth; memset(close,0,sizeof(close));
        close[0].kind=close[1].kind=TK_NAME;
        close[0].text=copy_text("END"); close[1].text=copy_text("IF");
        close[0].column=close[1].column=column;
        emit(close,2,line,0);
    }
}

static int alpha(int c) { return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || c=='_'; }
static int digit(int c) { return c>='0'&&c<='9'; }

void load_program(FILE *input) {
    Token tokens[1024];
    char text[BASIC_STRING+1];
    unsigned n=0, length=0, i;
    unsigned long line=1, col=1;
    int c, d, quoted, first=1;
    basic.code=allocate(BASIC_CODE*sizeof(Instruction)); basic.procedures=1;
    memset(&diagnostic,0,sizeof(diagnostic)); location=&diagnostic;
    c=source_char(input);
    while (c!=EOF) {
        diagnostic.line=line; location=&diagnostic; column=col;
        if (c==0 || c>127) fail("source must be ASCII text");
        if (c==' ' || c=='\t' || c=='\r') { ++col; c=source_char(input); continue; }
        if (c=='\'' || (n==0 && c=='\n')) {
            while (c!=EOF && c!='\n') { c=source_char(input); ++col;  }
            if (c==EOF) break;
        }
        if (c=='\n' || c==':') {
            int label=c==':' && first && n==1 && tokens[0].kind==TK_NAME;
            emit(tokens,n,line,label); n=0;
            first=c=='\n';
            if (c=='\n') { close_inline(line); ++line; col=1; } else ++col;
            c=source_char(input); continue;
        }
        if (n==1024 || ++total_tokens>BASIC_TOKENS) fail("too many source tokens");
        memset(&tokens[n],0,sizeof(Token)); tokens[n].column=col;
        length=0;
        if (c=='"') {
            tokens[n].kind=TK_STRING;
            c=source_char(input); ++col; quoted=1;
            while (c!=EOF && c!='\r' && c!='\n') {
                if (c=='"') {
                    d=source_char(input); ++col;
                    if (d!='"') { c=d; quoted=0; break; }
                }
                if (c==0 || c>127 || length==BASIC_STRING) fail("invalid or excessive string literal");
                text[length++]=(char)c; c=source_char(input); ++col;
            }
            if (quoted) fail("unterminated string literal");
            tokens[n++].text=copy_range(text,length);  continue;
        }
        if (alpha(c) || digit(c) || c=='.') {
            int numeric_token=digit(c) || c=='.';
            tokens[n].kind=numeric_token ? TK_NUMBER : TK_NAME;
            do {
                if (length>=(unsigned)(numeric_token?128:63)) fail("token too long");
                text[length++]=(char)(!numeric_token && c>='a'&&c<='z' ? c-32 : c);
                c=source_char(input); ++col;
                if (numeric_token && length && (text[length-1]=='e'||text[length-1]=='E') && (c=='+'||c=='-')) {
                    if (length>=128) fail("numeric literal too long");
                    text[length++]=(char)c; c=source_char(input); ++col;
                }
            } while (numeric_token ? digit(c)||c=='.'||c=='e'||c=='E' : alpha(c)||digit(c));
            if (!numeric_token && c=='$') { if (length==63) fail("name exceeds 63 characters"); text[length++]='$'; c=source_char(input); ++col; }
            text[length]=0;
            if (numeric_token) {
                char *end;
                tokens[n].number=strtod(text,&end);
                if (end==text || *end) fail("invalid numeric literal");
            } else tokens[n].text=copy_range(text,length);
            ++n;
            if (!numeric_token && is_word(&tokens[n-1],"REM")) {
                free(tokens[--n].text);
                while (c!=EOF && c!='\n') { c=source_char(input); ++col;  }
            }
            continue;
        }
        tokens[n++].kind=c; ++col;
        if (c=='<' || c=='>') {
            d=source_char(input);
            if (d=='=') { tokens[n-1].kind=c=='<' ? TK_LE : TK_GE; ++col; c=source_char(input); }
            else if (c=='<' && d=='>') { tokens[n-1].kind=TK_NE; ++col; c=source_char(input); }
            else c=d;
        } else c=source_char(input);
    }
    if (ferror(input)) fail("cannot read source");
    emit(tokens,n,line,0);
    close_inline(line);
    if (scope) fail("missing END SUB");
    /* Pair structured blocks once. Branches use instruction indices at runtime. */
    for (i=0;i<basic.count;++i) {
        Instruction *in=&basic.code[i];
        int op=in->op, expected, j;
        Block *b;
        location=in; column=in->tokens[0].column;
        if ((op==OP_SUB || op==OP_RETURN) && depth) fail("block crosses a SUB boundary");
        if (op==OP_IF || op==OP_SELECT || op==OP_FOR || op==OP_WHILE) {
            if (depth==BASIC_DEPTH) fail("block nesting exceeds 64");
            blocks[depth].start=blocks[depth].last=(int)i; blocks[depth++].saw_else=0;
        } else if (op==OP_ELSEIF || op==OP_ELSE || op==OP_CASE) {
            expected=op==OP_CASE ? OP_SELECT : OP_IF;
            if (!depth) fail("branch without enclosing block");
            b=&blocks[depth-1];
            if (basic.code[b->start].op!=expected || b->saw_else) fail("misplaced branch");
            basic.code[b->last].jump=(int)i; b->last=(int)i;
            if (op==OP_ELSE || (op==OP_CASE && in->count>1 && is_word(in->tokens+1,"ELSE"))) b->saw_else=1;
        } else if (op==OP_ENDIF || op==OP_ENDSELECT || op==OP_NEXT || op==OP_WEND) {
            expected=op==OP_ENDIF ? OP_IF : op==OP_ENDSELECT ? OP_SELECT : op==OP_NEXT ? OP_FOR : OP_WHILE;
            if (!depth) fail("block end without block start");
            b=&blocks[--depth];
            if (basic.code[b->start].op!=expected || basic.code[b->start].scope!=in->scope) fail("mismatched block end");
            j=b->start;
            while (j!=(int)i && j>=0) { int next=basic.code[j].jump; basic.code[j].end=(int)i; j=next; }
            basic.code[b->last].jump=(int)i;
            in->jump=b->start;
        }
    }
    if (depth) { location=&basic.code[blocks[depth-1].start]; fail("unclosed block"); }
    /* Scoped labels and procedure calls are bound before any program I/O. */
    for (i=0;i<basic.count;++i) {
        Instruction *in=&basic.code[i]; unsigned j;
        location=in; column=in->tokens[0].column;
        if (in->op==OP_GOTO || in->op==OP_LABEL) {
            const char *name;
            if (in->count!=(unsigned)(in->op==OP_GOTO?2:1) || in->tokens[in->count-1].kind!=TK_NAME) fail("invalid label");
            name=in->tokens[in->count-1].text;
            for (j=0;j<basic.count;++j) if (basic.code[j].op==OP_LABEL && basic.code[j].scope==in->scope && !strcmp(name,basic.code[j].tokens[0].text)) {
                if (in->op==OP_LABEL && j!=i) fail("duplicate label");
                in->jump=(int)j;
            }
            if (in->jump<0) fail("unknown label in this SUB");
        }
        if (in->op==OP_CALL) {
            if (in->count<2 || in->tokens[1].kind!=TK_NAME) fail("CALL requires a SUB name");
            for (j=1;j<basic.procedures;++j) if (!strcmp(in->tokens[1].text,basic.proc[j].name)) in->jump=(int)j;
            if (in->jump<0) fail("unknown SUB");
        }
    }
    location=NULL;
}

void free_program(void) {
    unsigned i,j;
    for (i=0;i<basic.count;++i) {
        for (j=0;j<basic.code[i].count;++j) free(basic.code[i].tokens[j].text);
        free(basic.code[i].tokens);
    }
    for (i=1;i<basic.procedures;++i) free(basic.proc[i].params);
    free(basic.code);
}
