use super::*;
#[test]
fn wide_accesses_gep_and_sparse_global_arrays_cross_banks() {
    let m = parse(
        r#"
@large = external [64010 x i16] = [89, 0, 90]
function i64 @wide() {
 block 0:
  store i64 -81985529216486896, fat(63998, heap(5))
  %0 = getelementptr i16*, i16 fat(63998, heap(5)), 3
  %1 = getelementptr i16*, i16 %0, -3
  %2 = load i64, %1
  ret %2
}
function i16 @globals() {
 block 0:
  %0 = getelementptr i16*, i16 global @large, 64005
  store i16 42, %0
  %1 = load i16, %0
  ret %1
}
"#,
    );
    let a = compile(&[m], &options(&["wide", "globals"])).unwrap();
    assert!(
        a.bytes.len() < 4096,
        "BSS must not become a zero-filled download"
    );
    run(
        &a,
        vec![
            case("wide", &[], 0xfedcba9876543210),
            case("globals", &[], 42),
        ],
    );
}
#[test]
fn real_c_globals_aggregate_pointers_callbacks_and_stack_frames() {
    let m = c(r#"
        struct pair { long value; int *pointer; };
        static int answer=42;
        static struct pair data[2]={{123456L,&answer},{987654L,&answer}};
        static int left(int x){return x+10;}
        static int right(int x){return x+20;}
        static int (*callbacks[2])(int)={left,right};
        int recurse(int n,int *ancestor){
            int value=7;
            if(n){int result=recurse(n-1,&value);if(value!=8)return -100;return result+1;}
            *ancestor=*ancestor+1;return 1;
        }
        int skipped(void){goto use;int value=9;use:value=42;return value;}
        int main(void){
            int value=0;
            if(*data[1].pointer!=42 || data[1].value!=987654L)return 1;
            if(callbacks[0](1)!=11 || callbacks[1](2)!=22)return 2;
            if(recurse(1,&value)!=2)return 3;
            if(skipped()!=42)return 4;
            return 42;
        }
    "#);
    run(
        &compile(&[m], &Options::default()).unwrap(),
        vec![case("main", &[], 42)],
    );
}
#[test]
fn allocation_reuse_coalescing_cross_bank_realloc_and_calloc() {
    let m = c(r#"
        typedef unsigned long size_t;
        extern void *malloc(size_t);extern void free(void *);
        extern void *calloc(size_t,size_t);extern void *realloc(void *,size_t);
        int main(void){
            unsigned *a=malloc(64020UL),*b,*saved;long i;
            if(!a)return 1;
            a[0]=123;a[64019L]=456;
            b=realloc(a,65000UL);if(!b || b[0]!=123 || b[64019L]!=456)return 2;
            free(b);
            a=calloc(64020UL,1UL);if(!a || a[0] || a[64019L])return 3;
            saved=a;free(a);b=malloc(64020UL);if(b!=saved)return 4;
            free(b);
            if(calloc(0xffffffffUL,2UL) || malloc(0UL))return 5;
            for(i=0;i<100;i++){a=malloc(32UL);b=malloc(64UL);if(!a||!b)return 6;free(a);free(b);}
            a=malloc(1000000UL);if(!a)return 7;free(a);
            return 42;
        }
    "#);
    let artifact = compile(&[m], &Options::default()).unwrap();
    run(
        &artifact,
        vec![case("main", &[], 42), case("main", &[], 42)],
    );
}
#[test]
fn invalid_logical_pointers_ranges_offsets_and_code_tokens_trap() {
    let m = parse(
        r#"
function i64 @load(i16* %0) { block 0:
 %1 = load i64, %0
 ret %1
}
function i16* @step(i16* %0, i64 %1) { block 0:
 %2 = getelementptr i16*, i64 %0, %1
 ret %2
}
"#,
    );
    let artifact = compile(&[m], &options(&["load", "step"])).unwrap();
    let mut cases = Vec::new();
    for ptr in [
        0,
        64000,
        0x1fa00,
        0x80000001,
        0x1000000,
        0x100000001,
        (255 << 16) | 63999,
    ] {
        cases.push(case("load", &[ptr], 1));
    }
    cases.extend([
        case("step", &[0x10000, u64::MAX], 2),
        case("step", &[0x50001, 1 << 60], 2),
    ]);
    traps(&artifact, cases);
}
#[test]
fn indirect_calls_check_tokens_and_arity_and_heap_rejects_double_free() {
    let m = c(r#"
        extern void *malloc(unsigned long);extern void free(void *);
        int main(void){void *p=malloc(20);free(p);free(p);return 0;}
    "#);
    traps(
        &compile(&[m], &Options::default()).unwrap(),
        vec![case("main", &[], 4)],
    );
    let m = parse(
        r#"
function i16 @callee() { block 0: ret 42 }
function i16* @token() { block 0: ret func @callee }
function i16 @invoke(i16* %0) { block 0:
 %1 = call i16 %0(1)
 ret %1
}
"#,
    );
    let a = compile(&[m], &options(&["invoke", "token"])).unwrap();
    // Alphabetical IR linking places callee at table slot one; the table's
    // exact private token encoding is tested here, not exposed as a host API.
    traps(
        &a,
        vec![
            case("invoke", &[0], 6),
            case("invoke", &[0x10001], 6),
            case("invoke", &[0x80000001], 6),
            case("invoke", &[0x8fffffff], 6),
        ],
    );
}

#[test]
fn fallback_runtime_respects_application_symbols_and_unit_private_names() {
    for source in [
        "long abs=42L;int main(void){return (int)abs;}",
        "extern void *malloc(unsigned long);extern void free(void *);int abs=42;int main(void){void *p=malloc(1);if(!p)return 1;free(p);return abs;}",
    ]{run(&compile(&[c(source)],&Options::default()).unwrap(),vec![case("main",&[],42)]);}
    let first = c("static int malloc(int n){return n;}int local(void){return malloc(20);}");
    let second=c("extern int local(void);extern void *malloc(unsigned long);extern void free(void *);int main(void){void *p=malloc(32);if(!p)return 1;free(p);return local()+22;}");
    run(
        &compile(&[first, second], &Options::default()).unwrap(),
        vec![case("main", &[], 42)],
    );
}

#[test]
fn compatibility_stack_exhaustion_traps_before_reaching_heap_banks() {
    let module=c("int dive(int n){volatile int pad[1024];pad[0]=n;return dive(n+1)+pad[0];}int main(void){return dive(0);}");
    traps(
        &compile(&[module], &Options::default()).unwrap(),
        vec![case("main", &[], 3)],
    );
}

#[test]
fn memory_and_string_helpers_preserve_full_c16_cells_and_overlap() {
    let module = c(r#"
        typedef unsigned short cell;typedef unsigned long size_t;
        extern int strcmp(const cell *,const cell *);extern int strncmp(const cell *,const cell *,size_t);
        extern int memcmp(const void *,const void *,size_t);extern void *memchr(const void *,int,size_t);
        extern void *memmove(void *,const void *,size_t);extern void *memset(void *,int,size_t);
        extern cell *strchr(const cell *,int);extern cell *strrchr(const cell *,int);
        int main(void){
            cell high[2]={65535U,0},low[2]={1,0},values[5]={1,2,3,4,0};
            if(strcmp(high,low)<=0 || strncmp(low,high,1UL)>=0 || memcmp(high,low,1UL)<=0)return 1;
            if(memchr(high,65535U,1UL)!=high || strchr(high,0)!=high+1 || strrchr(high,65535U)!=high)return 2;
            memmove(values+1,values,3UL);if(values[1]!=1 || values[2]!=2 || values[3]!=3)return 3;
            memmove(values,values+1,3UL);if(values[0]!=1 || values[1]!=2 || values[2]!=3)return 4;
            memset(values,65535U,4UL);if(values[0]!=65535U || values[3]!=65535U || values[4])return 5;
            return 42;
        }
    "#);
    run(
        &compile(&[module], &Options::default()).unwrap(),
        vec![case("main", &[], 42)],
    );
}
