/* SPDX-License-Identifier: MIT */
#include "basic.h"

static FILE *files[BASIC_FILES];
static unsigned print_column;
static int terminal, coloured;
static int stdin_after_cr;

void output_text(FILE *file, const char *s) {
    const char *p;
    if (check_only) return;
    if (fputs(s,file)==EOF) fail("output failed");
    if (file==stdout) for (p=s;*p;++p) print_column=*p=='\n'||*p=='\r' ? 0 : print_column+1;
}
static double number(Parser *p) { Value v=expression(p,1); double n=numeric(v); free_value(v); return n; }
static int condition(Parser *p) { Value v=expression(p,1); int n=truth(v); free_value(v); return n; }
static unsigned file_number(Parser *p) {
    double n=number(p);
    if (check_only) return 0;
    if (n<1 || n>BASIC_FILES || n!=floor(n)) fail("file number must be 1 through 16");
    return (unsigned)n-1;
}
static FILE *stream(Parser *p) {
    unsigned n=file_number(p);
    if (!check_only && !files[n]) fail("file is not open");
    return check_only ? stdout : files[n];
}
static void read_line(FILE *file, char *line) {
    unsigned n=0;
    int c,skip_lf=file==stdin ? stdin_after_cr : 0;
    if (file==stdin) stdin_after_cr=0;
    /* The freshly booted console can use CR as VEOL without ICRNL. Accept
     * CR, LF and CRLF without changing inherited terminal modes. Do not peek
     * for LF after a terminal CR: that would block until the next keystroke. */
    for (;;) {
        c=fgetc(file);
        if (skip_lf) { skip_lf=0; if (c=='\n') continue; }
        if (c==EOF) {
            if (ferror(file)) fail("input read failed");
            if (!n) fail("unexpected end of input");
            break;
        }
        if (c=='\n') break;
        if (c=='\r') {
            if (file==stdin) stdin_after_cr=1;
            else { c=fgetc(file); if (c!=EOF && c!='\n') ungetc(c,file); }
            break;
        }
        if (n==BASIC_STRING) fail("input line exceeds 4096 characters");
        if (!c) fail("NUL in input record");
        line[n++]=(char)c;
    }
    line[n]=0;
}
static char *field(char **cursor, int *more) {
    char *p=*cursor, text[BASIC_STRING+1];
    unsigned n=0;
    int closed=0;
    while (*p==' ' || *p=='\t') ++p;
    if (*p=='"') {
        ++p;
        while (*p) {
            if (*p=='"') {
                ++p;
                if (*p!='"') { closed=1; break; }
            }
            text[n++]=*p++;
        }
        if (!closed) fail("unterminated quoted input field");
        while (*p==' '||*p=='\t') ++p;
        if (*p && *p!=',') fail("unexpected text after quoted input field");
    } else {
        while (*p && *p!=',') text[n++]=*p++;
        while (n && (text[n-1]==' ' || text[n-1]=='\t')) --n;
    }
    *more=*p==',';
    if (*more) ++p;
    *cursor=p;
    return copy_range(text,n);
}
static void input_values(Parser *p) {
    FILE *file=stdin;
    Variable *vars[32]; Value values[32],prompt;
    char line[BASIC_STRING+1],*cursor,*text,*end;
    unsigned n=0,i; int more=0,bad=0,from_file=0;
    prompt=string_value("");
    if (accept_token(p,'#')) { file=stream(p); require(p,','); from_file=1; }
    else if (p->at<p->count && p->tokens[p->at].kind==TK_STRING) {
        free_value(prompt); prompt=expression(p,1);
        if (!prompt.string) fail("INPUT prompt must be a string");
        if (!accept_token(p,';')) require(p,',');
    }
    do {
        if (n==32) fail("INPUT exceeds 32 variables");
        vars[n++]=referent(variable(p->frame,name_token(p)->text));
    } while (accept_token(p,','));
    finished(p);
    if (check_only) { free_value(prompt); return; }
    do {
        if (!from_file) { output_text(stdout,prompt.string); output_text(stdout," "); fflush(stdout); }
        read_line(file,line); cursor=line; bad=0;
        for (i=0;i<n;++i) {
            text=field(&cursor,&more);
            if (vars[i]->value.string) values[i]=string_value(text);
            else {
                double a=*text ? strtod(text,&end) : 0;
                if (!*text) end=text;
                while (*end==' '||*end=='\t') ++end;
                if (*end) { a=0; bad=1; }
                values[i]=number_value(a);
            }
            free(text);
            if ((i+1<n && !more) || (i+1==n && more)) bad=1;
        }
        if (bad) {
            for (i=0;i<n;++i) free_value(values[i]);
            if (from_file) fail("invalid INPUT record");
            output_text(stdout,"?Redo from start\n");
        }
    } while (bad);
    for (i=0;i<n;++i) assign(vars[i],values[i]);
    free_value(prompt); print_column=0;
}
static void print_values(Parser *p, int write_record) {
    FILE *file=stdout;
    Value v;
    char *s; unsigned n,i;
    int trailing=0,first=1;
    if (accept_token(p,'#')) { file=stream(p); require(p,','); }
    while (p->at<p->count) {
        if (!write_record && word(p,"TAB")) {
            double a;
            require(p,'('); a=number(p); require(p,')');
            if (file!=stdout) fail("TAB is only supported on terminal output");
            if (!check_only) {
                if (a<1 || a>BASIC_STRING) fail("TAB column out of range");
                n=(unsigned)a-1;
                if (print_column>n) output_text(file,"\n");
                while (print_column<n) output_text(file," ");
            }
        } else {
            v=expression(p,1);
            if (write_record && !first) output_text(file,",");
            if (v.string) {
                if (write_record) {
                    output_text(file,"\"");
                    for (i=0;v.string[i];++i) {
                        char ch[2]; ch[0]=v.string[i]; ch[1]=0;
                        output_text(file,ch); if (ch[0]=='"') output_text(file,"\"");
                    }
                    output_text(file,"\"");
                } else output_text(file,v.string);
            } else {
                s=number_text(v.number,!write_record); output_text(file,s); free(s);
                if (!write_record) output_text(file," ");
            }
            free_value(v); first=0;
        }
        trailing=0;
        if (accept_token(p,';')) { if (write_record) fail("WRITE uses commas"); trailing=1; }
        else if (accept_token(p,',')) {
            if (!write_record && !check_only) { n=14-print_column%14; while (n--) output_text(file," "); }
            trailing=1;
        } else if (p->at<p->count) fail("expected PRINT separator");
    }
    if (!trailing || write_record) output_text(file,"\n");
}
static void open_file(Parser *p) {
    Value path=expression(p,1);
    const char *mode;
    unsigned n;
    if (!path.string) fail("OPEN requires a string path");
    require_word(p,"FOR");
    if (word(p,"INPUT")) mode="r";
    else if (word(p,"OUTPUT")) mode="w";
    else if (word(p,"APPEND")) mode="a";
    else { fail("OPEN supports INPUT, OUTPUT and APPEND"); mode="r"; }
    require_word(p,"AS"); accept_token(p,'#'); n=file_number(p); finished(p);
    if (!check_only) {
        if (files[n]) fail("file number is already open");
        files[n]=fopen(path.string,mode);
        if (!files[n]) { perror(path.string); fail("cannot open file"); }
    }
    free_value(path);
}
static void close_files(Parser *p) {
    unsigned i;
    if (p->at==p->count) {
        for (i=0;i<BASIC_FILES;++i) if (files[i]) {
            int failed=fclose(files[i]); files[i]=NULL;
            if (failed) fail("cannot close file");
        }
    } else do {
        accept_token(p,'#'); i=file_number(p);
        if (!check_only) {
            int failed;
            if (!files[i]) fail("file is not open");
            failed=fclose(files[i]); files[i]=NULL;
            if (failed) fail("cannot close file");
        }
    } while (accept_token(p,','));
    finished(p);
}
static int compare(Value a, Value b) {
    if ((a.string!=NULL)!=(b.string!=NULL)) fail("CASE value type mismatch");
    return a.string ? strcmp(a.string,b.string) : a.number<b.number?-1:a.number>b.number?1:0;
}
static int case_matches(Parser *p, Value subject) {
    Value low,high; int match=0;
    if (word(p,"ELSE")) { finished(p); return 1; }
    do {
        low=expression(p,1);
        if (word(p,"TO")) {
            high=expression(p,1);
            if (!check_only && compare(subject,low)>=0 && compare(subject,high)<=0) match=1;
            free_value(high);
        } else if (!check_only && !compare(subject,low)) match=1;
        free_value(low);
    } while (accept_token(p,','));
    finished(p); return match;
}
static Parser parser(Instruction *in, Frame *frame, unsigned skip) {
    Parser p; p.tokens=in->tokens; p.count=in->count; p.at=skip; p.frame=frame;
    return p;
}
static int branch(Instruction *in, Frame *frame) {
    Parser p=parser(in,frame,1);
    location=in; column=in->tokens[0].column;
    int yes=condition(&p);
    require_word(&p,"THEN"); finished(&p); return yes;
}
static Frame *call_sub(Parser *p, Procedure *proc, int return_pc) {
    Frame *callee=allocate(sizeof(Frame));
    unsigned n=0;
    callee->caller=p->frame; callee->return_pc=return_pc;
    require(p,'(');
    if (!accept_token(p,')')) {
        do {
            Variable *arg=NULL,*param;
            Value v;
            if (n==proc->count) fail("too many SUB arguments");
            if (p->at+1<p->count && p->tokens[p->at].kind==TK_NAME &&
                (p->tokens[p->at+1].kind==','||p->tokens[p->at+1].kind==')') &&
                !is_word(p->tokens+p->at,"RND") && !is_word(p->tokens+p->at,"TIMER") && !is_word(p->tokens+p->at,"GRAPHICSAVAILABLE")) {
                arg=referent(variable(p->frame,name_token(p)->text)); v=clone_value(arg->value);
            } else v=expression(p,1);
            param=variable(callee,proc->params[n++].text);
            assign(param,v); param->alias=arg;
        } while (accept_token(p,','));
        require(p,')');
    }
    if (n!=proc->count) fail("too few SUB arguments");
    finished(p); return callee;
}
static void reset_terminal(void) { if (coloured) fputs("\033[0m",stdout); }

void run_program(void) {
    Frame *frame=allocate(sizeof(Frame)),*frames[64],*next;
    Instruction *in;
    Parser p;
    Value v;
    Variable *var;
    int pc=0,j,depth=0,done=0;
    unsigned i,n;
    double a,b;
    memset(frames,0,sizeof(frames)); frames[0]=frame;
    print_column=0;
    terminal=isatty(1);
    if (!check_only) atexit(reset_terminal);
    while (!done && pc<(int)basic.count) {
        in=&basic.code[pc]; location=in; column=in->tokens[0].column;
        if (check_only) {
            if (!frames[in->scope]) frames[in->scope]=allocate(sizeof(Frame));
            frame=frames[in->scope];
        }
        p=parser(in,frame,1); ++pc;
        switch (in->op) {
        case OP_ASSIGN:
            p.at=is_word(in->tokens,"LET")?1:0;
            var=variable(frame,name_token(&p)->text); require(&p,'='); v=expression(&p,1); finished(&p); assign(var,v); break;
        case OP_PRINT: case OP_WRITE: print_values(&p,in->op==OP_WRITE); break;
        case OP_INPUT: input_values(&p); break;
        case OP_OPEN: open_file(&p); break;
        case OP_CLOSE: close_files(&p); break;
        case OP_IF:
            if (!branch(in,frame) && !check_only) {
                j=in->jump;
                while (basic.code[j].op==OP_ELSEIF && !branch(&basic.code[j],frame)) j=basic.code[j].jump;
                pc=basic.code[j].op==OP_ELSEIF || basic.code[j].op==OP_ELSE ? j+1 : j;
            }
            break;
        case OP_ELSEIF:
            if (check_only) branch(in,frame); else pc=in->end;
            break;
        case OP_ELSE: finished(&p); if (!check_only) pc=in->end; break;
        case OP_SELECT:
            require_word(&p,"CASE"); v=expression(&p,1); finished(&p);
            if (!check_only) {
                j=in->jump; pc=in->end+1;
                while (basic.code[j].op==OP_CASE) {
                    Parser choice=parser(&basic.code[j],frame,1);
                    location=&basic.code[j];
                    if (case_matches(&choice,v)) { pc=j+1; break; }
                    j=basic.code[j].jump;
                }
            }
            free_value(v); break;
        case OP_CASE:
            if (check_only) { v=number_value(0); case_matches(&p,v); }
            else pc=in->end;
            break;
        case OP_GOTO: if (!check_only) pc=in->jump; break;
        case OP_CALL:
            p.at=2;
            /* CALL name without parentheses is allowed for zero parameters. */
            if (p.at==p.count && !basic.proc[in->jump].count) { next=allocate(sizeof(Frame)); next->caller=frame; next->return_pc=pc; }
            else next=call_sub(&p,&basic.proc[in->jump],pc);
            if (check_only) free_frame(next);
            else {
                if (++depth>32) fail("SUB call depth exceeds 32");
                frame=next; pc=basic.proc[in->jump].entry;
            }
            break;
        case OP_SUB: if (!check_only) done=1; break;
        case OP_RETURN:
            p.at=2; finished(&p);
            if (!check_only) {
                if (!frame->caller) fail("END SUB outside a call");
                pc=frame->return_pc; next=frame->caller; free_frame(frame); frame=next; --depth;
            }
            break;
        case OP_END: finished(&p); if (!check_only) done=1; break;
        case OP_CLS: finished(&p); if (terminal) output_text(stdout,"\033[2J\033[H"); print_column=0; break;
        case OP_COLOR:
            a=number(&p); b=-1; if (accept_token(&p,',')) b=number(&p); finished(&p);
            if (!check_only && (a<0||a>15||b<-1||b>15)) fail("COLOR requires values 0 through 15");
            if (!check_only && terminal) {
                static const int colour[8]={0,4,2,6,1,5,3,7};
                printf("\033[%dm",((int)a>=8?90:30)+colour[(int)a%8]);
                if (b>=0) printf("\033[%dm",((int)b>=8?100:40)+colour[(int)b%8]);
                coloured=1;
            }
            break;
        case OP_LOCATE:
            a=number(&p); b=1; if (accept_token(&p,',')) b=number(&p); finished(&p);
            if (!check_only && (a<1||a>4096||b<1||b>4096)) fail("LOCATE position out of range");
            if (!check_only && terminal) printf("\033[%u;%uH",(unsigned)a,(unsigned)b);
            print_column=b>0 && b<=4096 ? (unsigned)b-1 : 0; break;
        case OP_SLEEP:
            a=p.at<p.count?number(&p):0; finished(&p);
            if (!check_only) {
                if (a<0 || a>86400) fail("SLEEP duration out of range");
                fflush(stdout);
                if (!a) { char line[BASIC_STRING+1]; read_line(stdin,line); print_column=0; }
                else sleep((unsigned)a);
            }
            break;
        case OP_RANDOMIZE:
            a=number(&p); finished(&p);
            if (!check_only) { if (a<0) a=-a; random_state=(uint32_t)fmod(a,4294967296.0); }
            break;
        case OP_FOR: {
            Loop *loop;
            var=referent(variable(frame,name_token(&p)->text)); require(&p,'='); a=number(&p);
            require_word(&p,"TO"); b=number(&p);
            for (i=0;i<frame->loops && frame->loop[i].pc!=pc-1;++i) {}
            if (i==BASIC_DEPTH) fail("too many active FOR loops");
            if (i==frame->loops) ++frame->loops;
            loop=&frame->loop[i]; loop->pc=pc-1; loop->variable=var; loop->limit=b;
            loop->step=word(&p,"STEP")?number(&p):1; finished(&p);
            assign(var,number_value(a));
            if (!check_only) {
                if (!loop->step) fail("FOR STEP cannot be zero");
                if (loop->step>0 ? a>b : a<b) { pc=in->end+1; frame->loop[i]=frame->loop[--frame->loops]; }
            }
            break;
        }
        case OP_NEXT: {
            Loop *loop;
            if (p.at<p.count) {
                Token *name=name_token(&p);
                if (basic.code[in->jump].count<2 || strcmp(name->text,basic.code[in->jump].tokens[1].text)) fail("NEXT variable does not match FOR");
            }
            finished(&p);
            if (check_only) {
                for (i=0;i<frame->loops && frame->loop[i].pc!=in->jump;++i) {}
                if (i<frame->loops) frame->loop[i]=frame->loop[--frame->loops];
                break;
            }
            for (i=0;i<frame->loops && frame->loop[i].pc!=in->jump;++i) {}
            if (i==frame->loops) fail("NEXT without an active FOR");
            loop=&frame->loop[i]; a=numeric(loop->variable->value)+loop->step;
            assign(loop->variable,number_value(a));
            if (loop->step>0 ? a<=loop->limit : a>=loop->limit) pc=in->jump+1;
            else frame->loop[i]=frame->loop[--frame->loops];
            break;
        }
        case OP_WHILE: a=condition(&p); finished(&p); if (!check_only && !a) pc=in->end+1; break;
        case OP_WEND: finished(&p); if (!check_only) pc=in->jump; break;
        case OP_ENDIF: case OP_ENDSELECT:
            p.at=2; finished(&p); break;
        case OP_LABEL: case OP_NOP: break;
        default: fail("invalid interpreter instruction");
        }
    }
    for (n=0;n<BASIC_FILES;++n) if (files[n]) {
        int failed=fclose(files[n]); files[n]=NULL;
        if (failed) fail("cannot close file at program end");
    }
    if (check_only) for (i=0;i<64;++i) { if (frames[i]) free_frame(frames[i]); }
    else { while (frame) { next=frame->caller; free_frame(frame); frame=next; } }
    location=NULL;
}
