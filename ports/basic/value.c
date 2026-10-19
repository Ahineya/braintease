/* SPDX-License-Identifier: MIT */
#include "basic.h"

Value number_value(double n) {
    Value v;
    if (!(n>=-1.7976931348623157e308 && n<=1.7976931348623157e308)) fail("numeric overflow");
    v.number=n; v.string=NULL; return v;
}
Value string_value(const char *s) { Value v; v.number=0; v.string=copy_text(s); return v; }
Value clone_value(Value v) { return v.string ? string_value(v.string) : number_value(v.number); }
void free_value(Value v) { free(v.string); }
double numeric(Value v) { if (v.string) fail("expected a number"); return v.number; }
long integer_value(Value v) {
    double n=numeric(v);
    if (n<-2147483648.0 || n>2147483647.0) fail("integer operand outside signed 32-bit range");
    return (long)n;
}
int truth(Value v) { return numeric(v)!=0; }
Variable *referent(Variable *v) { while (v->alias) v=v->alias; return v; }
Variable *variable(Frame *f, const char *name) {
    Variable *v;
    unsigned n=(unsigned)strlen(name);
    for (v=f->variables;v;v=v->next) if (!strcmp(v->name,name)) return v;
    if (f->count==BASIC_VARS) fail("scope exceeds 256 variables");
    ++f->count; v=allocate(sizeof(Variable)); v->name=copy_text(name);
    if (n && name[n-1]=='$') v->value=string_value("");
    v->next=f->variables; f->variables=v; return v;
}
void assign(Variable *v, Value value) {
    v=referent(v);
    if ((v->value.string!=NULL)!=(value.string!=NULL)) fail("string/numeric assignment mismatch");
    free_value(v->value); v->value=value;
}
void free_frame(Frame *f) {
    Variable *v=f->variables,*next;
    while (v) { next=v->next; free_value(v->value); free(v->name); free(v); v=next; }
    free(f);
}
char *number_text(double n, int leading_space) {
    char text[64];
    if (leading_space && n>=0) snprintf(text,sizeof(text)," %.15g",n);
    else snprintf(text,sizeof(text),"%.17g",n);
    return copy_text(text);
}

static Value builtin(const char *name, Value *v, unsigned n) {
    char *s; unsigned i,len,begin,count; double a;
    Value result;
    if (!strcmp(name,"RND")) {
        if (n>1) fail("RND accepts zero or one argument");
        if (n) numeric(v[0]);
        if (check_only) return number_value(0);
        if (n && numeric(v[0])<0) random_state=0UL-(uint32_t)integer_value(v[0]);
        if (!n || numeric(v[0])!=0) random_state=random_state*1664525UL+1013904223UL;
        return number_value((double)(random_state>>8)/16777216.0);
    }
    if (!strcmp(name,"TIMER")) {
        if (n) fail("TIMER takes no arguments");
        return number_value((double)(time(NULL)%86400L));
    }
    if (!strcmp(name,"GRAPHICSAVAILABLE")) {
        if (n) fail("GRAPHICSAVAILABLE takes no arguments");
        return number_value(0);
    }
    if (!strcmp(name,"INT") || !strcmp(name,"FIX") || !strcmp(name,"ABS") || !strcmp(name,"SGN") || !strcmp(name,"STR$")) {
        if (n!=1) fail("numeric function requires one argument");
        a=numeric(v[0]);
        if (!strcmp(name,"STR$")) { result.number=0; result.string=number_text(a,1); return result; }
        return number_value(!strcmp(name,"INT") ? floor(a) : !strcmp(name,"FIX") ? (a<0?-floor(-a):floor(a)) :
                            !strcmp(name,"ABS") ? (a<0?-a:a) : a<0?-1:a>0?1:0);
    }
    if (n<1 || !v[0].string) fail("unknown function or invalid string argument");
    len=(unsigned)strlen(v[0].string);
    if (!strcmp(name,"LEN")) {
        if (n!=1) fail("LEN requires one argument");
        return number_value((double)len);
    }
    if (!strcmp(name,"UCASE$") || !strcmp(name,"LCASE$")) {
        if (n!=1) fail("case conversion requires one argument");
        s=copy_text(v[0].string);
        for (i=0;i<len;++i) {
            if (!strcmp(name,"UCASE$") && s[i]>='a' && s[i]<='z') s[i]-=32;
            if (!strcmp(name,"LCASE$") && s[i]>='A' && s[i]<='Z') s[i]+=32;
        }
        result.number=0; result.string=s; return result;
    }
    if (!strcmp(name,"LEFT$") || !strcmp(name,"RIGHT$") || !strcmp(name,"MID$")) {
        if (n<2 || n>3 || (n==3 && strcmp(name,"MID$"))) fail("invalid substring arguments");
        a=numeric(v[1]);
        if (check_only) a=1;
        if (a<0 || a>BASIC_STRING) fail("substring position/count out of range");
        count=(unsigned)a; begin=0;
        if (!strcmp(name,"MID$")) {
            if (!count) fail("MID$ positions start at one");
            begin=count-1; count=len;
            if (n==3) { a=numeric(v[2]); if (check_only) a=1; if (a<0 || a>BASIC_STRING) fail("substring count out of range"); count=(unsigned)a; }
        } else if (!strcmp(name,"RIGHT$")) begin=count<len ? len-count : 0;
        if (begin>len) begin=len;
        if (count>len-begin) count=len-begin;
        result.number=0; result.string=copy_range(v[0].string+begin,count); return result;
    }
    fail("unknown function"); return number_value(0);
}

static int precedence(Token *t) {
    if (is_word(t,"OR")) return 1;
    if (is_word(t,"XOR")) return 2;
    if (is_word(t,"AND")) return 3;
    if (t->kind=='=' || t->kind=='<' || t->kind=='>' || t->kind==TK_LE || t->kind==TK_GE || t->kind==TK_NE) return 4;
    if (t->kind=='+' || t->kind=='-') return 5;
    if (t->kind=='*' || t->kind=='/' || t->kind=='\\' || is_word(t,"MOD")) return 6;
    if (t->kind=='^') return 7;
    return 0;
}

static Value binary(Token *op, Value left, Value right) {
    double a,b,out=0;
    int cmp,k=op->kind,rank=precedence(op);
    Value result;
    char *s;
    unsigned n,m;
    if (left.string || right.string) {
        if (!left.string || !right.string) fail("cannot mix strings and numbers");
        if (k=='+') {
            n=(unsigned)strlen(left.string); m=(unsigned)strlen(right.string);
            if (n+m>BASIC_STRING) fail("string exceeds 4096 characters");
            s=allocate((size_t)n+m+1); memcpy(s,left.string,n); memcpy(s+n,right.string,m);
            result.number=0; result.string=s; return result;
        }
        if (rank!=4) fail("invalid string operator");
        cmp=strcmp(left.string,right.string);
    } else {
        a=left.number; b=right.number; cmp=a<b?-1:a>b?1:0;
        if (rank==4) {}
        else if (check_only) return number_value(1);
        else if (k=='+') out=a+b;
        else if (k=='-') out=a-b;
        else if (k=='*') out=a*b;
        else if (k=='/') { if (!b) fail("division by zero"); out=a/b; }
        else if (k=='^') out=pow(a,b);
        else if (k=='\\' || is_word(op,"MOD")) {
            long x=integer_value(left),y=integer_value(right);
            if (!y) fail("division by zero");
            if (x==(-2147483647L-1L) && y==-1) fail("integer division overflow");
            out=k=='\\' ? (double)(x/y) : (double)(x%y);
        } else {
            int32_t x=(int32_t)integer_value(left),y=(int32_t)integer_value(right);
            out=(double)(is_word(op,"AND") ? x&y : is_word(op,"OR") ? x|y : x^y);
        }
        if (rank!=4) return number_value(out);
    }
    out=k=='=' ? cmp==0 : k=='<' ? cmp<0 : k=='>' ? cmp>0 : k==TK_LE ? cmp<=0 : k==TK_GE ? cmp>=0 : cmp!=0;
    return number_value(out?-1:0);
}

Value expression(Parser *p, int minimum) {
    Value left,right,result,args[3];
    Token *t,*op;
    unsigned n=0;
    int rank,kind;
    static unsigned nesting;
    if (++nesting>64) fail("expression nesting exceeds 64");
    if (p->at>=p->count) fail("expected expression");
    t=&p->tokens[p->at++]; column=t->column; kind=t->kind;
    if (kind==TK_STRING) left=string_value(t->text);
    else if (kind==TK_NUMBER) left=number_value(t->number);
    else if (kind=='(') { left=expression(p,1); require(p,')'); }
    else if (kind=='+' || kind=='-' || is_word(t,"NOT")) {
        right=expression(p,is_word(t,"NOT")?4:7);
        left=number_value(is_word(t,"NOT") ? (double)(~(int32_t)integer_value(right)) : kind=='-' ? -numeric(right) : numeric(right));
        free_value(right);
    } else if (kind==TK_NAME) {
        if (accept_token(p,'(')) {
            if (!accept_token(p,')')) {
                do { if (n==3) fail("too many function arguments"); args[n++]=expression(p,1); } while (accept_token(p,','));
                require(p,')');
            }
            left=builtin(t->text,args,n);
            while (n) free_value(args[--n]);
        } else if (is_word(t,"RND") || is_word(t,"TIMER") || is_word(t,"GRAPHICSAVAILABLE")) left=builtin(t->text,args,0);
        else left=clone_value(referent(variable(p->frame,t->text))->value);
    } else { fail("expected expression"); left=number_value(0); }
    while (p->at<p->count) {
        op=&p->tokens[p->at]; rank=precedence(op);
        if (!rank || rank<minimum) break;
        ++p->at; column=op->column;
        right=expression(p,rank+(op->kind=='^'?0:1));
        result=binary(op,left,right); free_value(left); free_value(right); left=result;
    }
    --nesting; return left;
}
