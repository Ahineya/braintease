#[path = "support/fp.rs"]
mod support;

use rcc_ir::ir::{Instruction, Value};
use std::collections::HashSet;

#[test]
fn known_callback_frame_sizes_are_static() {
    let mut module = rfl::compile_to_ir("fn main(){let f=fn(x:i16)=>x+1;println(f(41));}").unwrap();
    rcc_backend::optimize::optimize_module(&mut module);
    let main = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_main")
        .unwrap();
    let allocations: Vec<_> = main
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            Instruction::Call {
                function: Value::Global(name),
                args,
                ..
            } if name == "rfl_frame_alloc" => Some(args),
            _ => None,
        })
        .collect();
    // Wide constants retain a typed materialization so the VM ABI knows that
    // both words must be passed. They need not become untyped Value::Constant.
    let static_sizes: HashSet<_> = main
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            Instruction::Binary {
                result,
                lhs: Value::Constant(_),
                rhs: Value::Constant(_),
                ..
            } => Some(*result),
            _ => None,
        })
        .collect();
    let direct = main.blocks.iter().flat_map(|b| &b.instructions).any(|i| {
        matches!(i,
        Instruction::Call{function:Value::Global(name),..} if name.starts_with("rfl_direct_"))
    });
    assert!(
        !allocations.is_empty() || direct,
        "the callback must actually be invoked"
    );
    assert!(allocations.iter().all(|args| match args.as_slice(){[Value::Constant(_)]=>true,[Value::Temp(t)]=>static_sizes.contains(t),_=>false}),
        "a statically known callback must not read its frame size from a closure value: {allocations:?}");
}

#[test]
fn direct_parameters_immutable_captures_and_container_fields_preserve_callbacks() {
    support::parity(
        r#"
        struct Holder{f:fn(i16)->i16,n:i16}
        type Wrapped=Wrapped(fn(i16)->i16);
        fn invoke(f:fn(i16)->i16,n:i16)->i16{f(n)}
        fn main(){
            let add=fn(n:i16)=>n+1;
            println(invoke(add,40));
            let nested=fn(n:i16)=>add(n);println(nested(41));
            let record=Holder{f:add,n:42};let selected=record.f;println(selected(record.n));
            match Wrapped(add){Wrapped(f)=>println(f(43))};
        }
    "#,
        "41\n42\n43\n44\n",
    );
}

#[test]
fn conflicting_callers_branches_loops_and_mutable_captures_remain_dynamic() {
    support::parity(
        r#"
        fn a(n:i16)->i16{n+1} fn b(n:i16)->i16{n+2}
        fn invoke(f:fn(i16)->i16)->i16{f(40)}
        fn choose(flag:bool)->i16{let f=if(flag){a}else{b};f(40)}
        fn main(){
            println(invoke(a));println(invoke(b));println(choose(true));println(choose(false));
            let mut changed=false;
            let change=fn()=>{changed=true;};
            let run=fn()=>{let selected=if(changed){b}else{a};selected(40)};
            println(run());change();println(run());
            let mut g:fn(i16)->i16=a;let mut n=0;
            while(n<3){println(g(40));g=b;n+=1;}
        }
    "#,
        "41\n42\n41\n42\n41\n42\n41\n42\n42\n",
    );
}

#[test]
fn owned_callbacks_keep_environment_and_cleanup_order() {
    support::parity(
        r#"
        fn invoke(f:once fn(i16)->i16)->i16{f(40)}
        fn main(){
            let owner=owned(2,fn(n:&mut i16)=>{println("drop");});
            let f=once fn(n:i16)=>n+*owner.borrow();
            println(invoke(move(f)));
        }
    "#,
        "drop\n42\n",
    );
}

fn dynamic_frame_sizes(source: &str, function: &str) -> usize {
    let module = rfl::compile_to_ir(source).unwrap();
    let body = module
        .functions
        .iter()
        .find(|f| f.name == function)
        .unwrap();
    let typed_constants: HashSet<_> = body
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            Instruction::Binary {
                result,
                lhs: Value::Constant(_),
                rhs: Value::Constant(_),
                ..
            } => Some(*result),
            _ => None,
        })
        .collect();
    body.blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter(|i| match i {
            Instruction::Call {
                function: Value::Global(name),
                args,
                ..
            } if name == "rfl_frame_alloc" => {
                !matches!(args.as_slice(),[Value::Temp(t)] if typed_constants.contains(t))
            }
            _ => false,
        })
        .count()
}

#[test]
fn only_agreeing_direct_callers_specialize_callback_parameters() {
    let prefix =
        "fn a(n:i16)->i16{n+1} fn b(n:i16)->i16{n+2} fn invoke(f:fn(i16)->i16)->i16{f(40)}";
    assert_eq!(
        dynamic_frame_sizes(
            &format!("{prefix} fn main(){{println(invoke(a));println(invoke(a));}}"),
            "rfl_fn_invoke"
        ),
        0
    );
    assert_eq!(
        dynamic_frame_sizes(
            &format!("{prefix} fn main(){{println(invoke(a));println(invoke(b));}}"),
            "rfl_fn_invoke"
        ),
        1
    );
}

#[test]
fn closure_cleanup_uses_logarithmic_dispatch() {
    let mut source = String::from("fn main(){let n=1;let mut sum=0;");
    for i in 0..32 {
        source.push_str(&format!("let f{i}=fn()=>n+{i};sum+=f{i}();"));
    }
    source.push_str("println(sum);}");
    let module = rfl::compile_to_ir(&source).unwrap();
    let cleanup = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_drop_func")
        .unwrap();
    fn depth(function: &rcc_ir::ir::Function, id: u32) -> usize {
        let block = function.blocks.iter().find(|b| b.id == id).unwrap();
        match block.instructions.last().unwrap() {
            Instruction::Branch(label) => depth(function, *label),
            Instruction::BranchCond {
                true_label,
                false_label,
                ..
            } => 1 + depth(function, *true_label).max(depth(function, *false_label)),
            _ => 0,
        }
    }
    let comparisons = depth(cleanup, cleanup.blocks[0].id);
    assert!(
        comparisons <= 10,
        "32 capturing functions should not require a linear scan: {comparisons} comparisons"
    );
    support::parity(&source, "528\n");
}
