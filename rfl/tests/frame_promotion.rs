#[path = "support/fp.rs"]
mod support;

use rcc_ir::ir::{Instruction, Value};
use rfl::Module;
use std::collections::HashMap;

fn temporary_frame_entry_loads(module: &Module) -> usize {
    let main = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_main")
        .unwrap();
    let entry = &main.blocks[0];
    let offsets: HashMap<_, _> = entry
        .instructions
        .iter()
        .filter_map(|instruction| {
            let Instruction::GetElementPtr {
                result,
                ptr: Value::FatPtr(pointer),
                indices,
                ..
            } = instruction
            else {
                return None;
            };
            if *pointer.addr != Value::Temp(0) {
                return None;
            }
            match indices.as_slice() {
                [Value::Constant(offset)] => Some((*result, *offset)),
                _ => None,
            }
        })
        .collect();
    entry
        .instructions
        .iter()
        .filter(|instruction| {
            let Instruction::Load {
                ptr: Value::FatPtr(pointer),
                ..
            } = instruction
            else {
                return false;
            };
            let Value::Temp(address) = *pointer.addr else {
                return false;
            };
            // Header occupies 0..5, and the i16 return slot is at 5.
            offsets.get(&address).is_some_and(|offset| *offset >= 6)
        })
        .count()
}

#[test]
fn write_only_scalar_slots_are_not_reloaded_at_function_entry() {
    let module = rfl::compile_to_ir("fn main()->i16 {let a=20; let b=22; let c=a+b; c}").unwrap();
    assert_eq!(
        temporary_frame_entry_loads(&module),
        0,
        "scalar temporaries defined by this invocation must not be read from the heap first"
    );
}

#[test]
fn continuation_values_are_not_reloaded_before_selecting_the_resume_entry() {
    let module = rfl::compile_to_ir(
        "fn step(x:i16)->i16{x+1} fn main()->i16 {let a=step(19);let b=step(21);a+b}",
    )
    .unwrap();
    assert_eq!(
        temporary_frame_entry_loads(&module),
        0,
        "the common dispatch entry must not reload values belonging to other continuations"
    );
}

#[test]
fn parameters_call_results_loop_state_and_captures_survive_resume() {
    support::parity(
        r#"
        fn add(x:i16)->i16 {x+1}
        fn run(seed:i16)->i16 {
            let mut sum=seed;
            let mut i=0;
            while (i<4) {
                let step=add(i);
                sum+=step;
                i+=1;
            }
            let f=fn(x:i16)=>sum+x;
            f(add(2))
        }
        fn main() {println(run(29));}
    "#,
        "42\n",
    );
}

#[test]
fn wide_descriptor_copies_preserve_raw_words_references_and_ownership() {
    support::parity(
        r#"
        struct Data{a:u32,b:u64,c:i16}
        fn pass(x:Data)->Data{x}
        fn text(x:String)->String{x}
        fn callback(n:i16)->fn()->i16{fn()=>n}
        fn main(){
            let d=pass(Data{a:4294967295,b:(0 as u64)-(1 as u64),c:-123});
            println(d.a);println(d.b);println(d.c);
            let s="hello";println(text(s));println(s);
            let mut callbacks:Vec<fn()->i16> = [];
            for(n in 0..512){callbacks.push(callback(n));}
            let mut sum:i32=0;
            for(f in callbacks){sum+=f() as i32;}
            println(sum);
        }
    "#,
        "4294967295\n18446744073709551615\n-123\nhello\nhello\n130816\n",
    );
}

#[test]
fn closure_descriptor_copies_use_wide_memory_operations() {
    let module =
        rfl::compile_to_ir("fn main(){let f=fn(n:i16)=>n+1;let g=f;println(g(41));}").unwrap();
    let main = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_main")
        .unwrap();
    assert!(
        main.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(
                i,
                Instruction::Load {
                    result_type: rcc_ir::ir::IrType::I64,
                    ..
                }
            )),
        "a four-cell descriptor should be copied with one bank-aware wide load"
    );
}

#[test]
fn borrowed_fields_keep_pointer_pairs_in_pointer_ir() {
    let module=rfl::compile_to_ir("struct Point{x:i16} fn read(p:&Point)->i16{p.x} fn main(){let p=Point{x:42};println(read(&p));}").unwrap();
    let read = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_read")
        .unwrap();
    assert!(!read.blocks.iter().flat_map(|b|&b.instructions).any(|i|matches!(i,Instruction::Unary{op:rcc_ir::ir::IrUnaryOp::IntToPtr,..})),
        "a stored reference already is an address/bank pair and should not be repacked through integer shifts");
}

#[test]
fn recursive_frame_copies_cross_ram_banks() {
    support::parity(
        r#"
        struct Payload{f:fn(i16)->i16,a:u64,b:u32,c:i16}
        fn nested(n:i16,p:Payload)->Payload{
            if(n==0){p}else{let answer=nested(n-1,p);let copy=answer;copy}
        }
        fn main(){
            let p=nested(1600,Payload{f:fn(n:i16)=>n+1,a:(0 as u64)-(1 as u64),b:4294967295,c:-123});
            let f=p.f;println(f(41));println(p.a);println(p.b);println(p.c);
        }
    "#,
        "42\n18446744073709551615\n4294967295\n-123\n",
    );
}
