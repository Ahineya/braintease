#[path = "support/fp.rs"]
mod support;
use rcc_ir::ir::{Instruction, Value};

fn heap_calls(source: &str, name: &str) -> usize {
    let module = rfl::compile_to_ir(source).unwrap();
    let body = module.functions.iter().find(|f| f.name == name).unwrap();
    body.blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter(
            |i| matches!(i,Instruction::Call{function:Value::Global(n),..} if n=="rfl_frame_alloc"),
        )
        .count()
}
#[test]
fn bounded_helpers_do_not_allocate_heap_activations() {
    assert_eq!(heap_calls("fn add(x:i16)->i16{x+1} fn twice(x:i16)->i16{let y=add(x);y+1} fn main()->i16{twice(40)}","rfl_fn_main"),0);
}
#[test]
fn loops_branches_wide_results_references_and_managed_returns() {
    support::parity(
        r#"
        struct Pair{a:u32,b:String}
        fn add(n:i16)->i16{if(n<0){return 0;}n+1}
        fn text(s:String)->String{s+"!"}
        fn pair(n:u32)->Pair{Pair{a:n+1,b:"hi"}}
        fn bump(n:&mut i16){*n+=1;}
        fn main(){let mut sum=0;for(n in 0..5){sum+=add(n);}bump(&mut sum);println(sum);
        let s="hello";println(text(s));println(s);let p=pair(4294967294);println(p.a);println(p.b);}
    "#,
        "16\nhello!\nhello\n4294967295\nhi\n",
    );
}
#[test]
fn recursive_and_unknown_callback_paths_keep_the_driver() {
    assert!(heap_calls("fn recur(n:i16)->i16{if(n==0){0}else{let x=recur(n-1);x+1}} fn main()->i16{recur(2000)}","rfl_fn_recur")>0);
    support::parity("fn a(x:i16)->i16{x+1} fn b(x:i16)->i16{x+2} fn apply(f:fn(i16)->i16)->i16{f(40)} fn main(){println(apply(a));println(apply(b));}","41\n42\n");
}
#[test]
fn resource_finalizers_keep_order_around_optimized_helpers() {
    support::parity(
        r#"fn step(n:i16)->i16{n+1} fn work(n:Owned<i16>)->i16{let value=step(*n.borrow());println("body");value} fn main(){let n=owned(41,fn(n:&mut i16)=>{println("drop");});println(work(move(n)));}"#,
        "body\ndrop\n42\n",
    );
}

#[test]
fn whole_owner_moves_and_returns_do_not_retain_the_same_share() {
    let module=rfl::compile_to_ir("fn keep(x:Owned<i16>)->Owned<i16>{move(x)} fn main(){let x=owned(42,fn(x:&mut i16)=>{});let x=keep(move(x));println(*x.borrow());}").unwrap();
    let keep = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_keep")
        .unwrap();
    assert!(
        !keep.blocks.iter().flat_map(|b| &b.instructions).any(
            |i| matches!(i,Instruction::Call{function:Value::Global(n),..} if n=="rfl_retain")
        ),
        "transferring a complete owner needs no temporary refcount increment"
    );
}

#[test]
fn direct_question_mark_and_indirect_managed_results_agree() {
    support::parity(
        r#"
        fn pass(v:Result<String,String>)->Result<String,String>{let s=v?;Ok(s+"!")}
        fn a()->String{"a"} fn b()->String{"b"}
        fn apply(f:fn()->String)->String{f()}
        fn main(){
            match pass(Ok("yes")){Ok(s)=>println(s),Err(s)=>println(s)};
            match pass(Err("no")){Ok(s)=>println(s),Err(s)=>println(s)};
            println(apply(a));println(apply(b));
        }
    "#,
        "yes!\nno\na\nb\n",
    );
}

#[test]
fn captured_values_and_reused_scratch_are_initialized_each_invocation() {
    support::parity(
        r#"
        fn message(n:i16)->String{if(n==0){"zero"}else{"other"}}
        fn main(){
            let mut n=40;let f=fn()=>{n+=1;n};
            println(f());println(f());println(n);
            let suffix="!";let text=fn(s:String)=>s+suffix;
            println(text("one"));println(text("two"));
            for(i in 0..3){println(message(i));}
        }
    "#,
        "41\n42\n42\none!\ntwo!\nzero\nother\nother\n",
    );
}

#[test]
fn deep_call_graphs_and_large_frames_fall_back_to_resumable_calls() {
    let mut source = String::from("fn f0(x:i16)->i16{x+1}");
    for i in 1..10 {
        source.push_str(&format!("fn f{i}(x:i16)->i16{{let y=f{}(x);y+1}}", i - 1));
    }
    source.push_str("fn main(){println(f9(32));}");
    let module = rfl::compile_to_ir(&source).unwrap();
    assert!(module.functions.iter().any(|f| f.name == "rfl_direct_f0"));
    assert!(!module.functions.iter().any(|f| f.name == "rfl_direct_f4"));
    support::parity(&source, "42\n");
    let source = format!(
        "fn big()->i16{{let data:[i16;300]=[{}];data[299]}} fn main(){{println(big());}}",
        vec!["1"; 300].join(",")
    );
    let module = rfl::compile_to_ir(&source).unwrap();
    assert!(!module.functions.iter().any(|f| f.name == "rfl_direct_big"));
    support::parity(&source, "1\n");
}

#[test]
fn optimized_helpers_execute_on_the_brainfuck_cpu() {
    support::brainfuck("fn step(n:i16)->i16{n+1} fn main(){let mut n=39;for(i in 0..3){n=step(n);}if(n==42){putc(89);}else{putc(78);}}","Y");
}
