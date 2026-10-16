/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C99 parser for the upstream find.y grammar: !, implicit/explicit AND, OR.
 * Read a primary's operand before requesting another token from yylex. */
static int lookahead = -1;
static int peek(void) {
    if (lookahead < 0) lookahead=yylex();
    return lookahead;
}
static void take(void) { lookahead=-1; }
static char *argument(void) {
    char *value=next();
    if (!value) yyerror("missing operand");
    return value;
}
static NODE *expression(void);
static NODE *primary(void) {
    int token=peek();
    NODE *n=NULL;
    take();
    switch (token) {
    case '(':
        n=expression();
        if (peek()!=')') yyerror("missing )");
        take(); return n;
    case '!': return bnode('!',primary(),NULL);
    case NAME: return snode(xname,argument());
    case PERM: return onode(xperm);
    case TYPE: return snode(xtype,argument());
    case LINKS: return nnode(xlinks);
    case USER: return getuser();
    case GROUP: return getgroup();
    case SIZE: return nnode(xsize);
    case INUM: return nnode(xinum);
    case ATIME: return nnode(xatime);
    case CTIME: return nnode(xctime);
    case MTIME: return nnode(xmtime);
    case EXEC: return enode(0);
    case OK: return enode(1);
    case PRINT: seflag++; return snode(xprint,NULL);
    case NEWER: return getnewer();
    case NOP: seflag++; return snode(xnop,NULL);
    default: yyerror("expected primary"); return NULL;
    }
}
static NODE *conjunction(void) {
    NODE *n=primary();
    while (peek()==AND) {
        NODE *right;
        take(); right=primary(); n=bnode(AND,n,right);
    }
    return n;
}
static NODE *expression(void) {
    NODE *n=conjunction();
    while (peek()==OR) {
        NODE *right;
        take(); right=conjunction(); n=bnode(OR,n,right);
    }
    return n;
}
int yyparse(void) {
    if (peek()=='\n') code=snode(xprint,NULL);
    else {
        code=expression();
        if (peek()!='\n') yyerror("trailing expression");
        if (!seflag) code=bnode(AND,code,snode(xprint,NULL));
    }
    return 0;
}
