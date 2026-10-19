/* Host execution of the generated frame adaptation, including allocation failure. */
#include <stdio.h>
#include <string.h>
#include "quickjs.h"
static unsigned checks, failures, callbacks;
#define C(x) do { checks++; if(!(x)) { failures++; printf("FAIL %u\n",__LINE__); } } while(0)
static JSValue native(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv; callbacks++; return JS_NewInt32(ctx,42);
}
static JSValue data(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                     int magic, JSValue *values) {
    (void)magic; (void)values; return native(ctx,self,argc,argv);
}
int main(void) {
    JSRuntime *rt=JS_NewRuntime();
    JSContext *ctx=JS_NewContext(rt);
    JSValue funcs[4],value,error;
    unsigned i, before;
    funcs[0]=JS_Eval(ctx,"(function(a,b,c){return 42;})",strlen("(function(a,b,c){return 42;})"),"test",0);
    funcs[1]=JS_NewCFunction(ctx,native,"native",3);
    funcs[2]=JS_NewCFunctionData(ctx,data,3,0,0,NULL);
    funcs[3]=JS_Eval(ctx,"(function(a){return a;}).bind(null,42)",strlen("(function(a){return a;}).bind(null,42)"),"test",0);
    for(i=0;i<4;i++) {
        C(!JS_IsException(funcs[i]));
        JS_SetMemoryLimit(rt,0); before=callbacks;
        value=JS_Call(ctx,funcs[i],JS_UNDEFINED,0,NULL);
        C(JS_IsException(value)); C(callbacks==before);
        error=JS_GetException(ctx); JS_FreeValue(ctx,error); JS_FreeValue(ctx,value);
        JS_SetMemoryLimit(rt,1024*1024);
        value=JS_Call(ctx,funcs[i],JS_UNDEFINED,0,NULL);
        C(!JS_IsException(value) && JS_IsNumber(value) && JS_VALUE_GET_INT(value)==42);
        JS_FreeValue(ctx,value); JS_FreeValue(ctx,funcs[i]);
    }
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    printf("QUICKJS_FRAMES checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
