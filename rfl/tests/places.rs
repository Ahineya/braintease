//! Borrowed places must address original storage on every target.
use std::path::{Path, PathBuf};

struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let sequence = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let directory = std::env::temp_dir().join(format!(
            "rfl-places-{}-{stamp}-{sequence}",
            std::process::id()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("main.rfl");
        std::fs::write(&path, source).unwrap();
        Self(path)
    }
    fn path(&self) -> &Path {
        &self.0
    }
}
impl Drop for Program {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn parity(source: &str, status: i32, stdout: &str) {
    let program = Program::new(source);
    for result in [
        rfl::run_native_file(program.path(), false).unwrap_or_else(|e| panic!("{e}")),
        rfl::run_file(program.path()).unwrap_or_else(|e| panic!("{e}")),
    ] {
        assert_eq!(result.status, status, "{}", result.stdout);
        assert_eq!(result.stdout, stdout);
    }
}

#[test]
fn nested_fields_are_borrowed_in_place() {
    parity(
        r#"
        struct Child { value:i32 }
        struct Parent { child:Child, other:i16 }
        fn change(x:&mut i32) { *x=123456; }
        fn visit(p:&mut Parent) { change(&mut p.child.value); }
        fn read(x:&i32)->i32 { *x }
        fn main()->i16 {
            let mut p=Parent{child:Child{value:7},other:8};
            visit(&mut p);
            if (read(&p.child.value) != 123456 || p.other != 8) { return 1; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn nested_vector_element_borrows_preserve_cow_and_managed_values() {
    parity(
        r#"
        struct Entry { text:String, numbers:Vec<i32> }
        fn replace(s:&mut String) { *s="new"; }
        fn number(n:&mut i32) { *n=123456; }
        fn main()->i16 {
            let mut rows=[Entry{text:"old",numbers:[7,8]}];
            let copy=rows;
            let view=rows[0..1];
            replace(&mut rows[0].text);
            number(&mut rows[0].numbers[1]);
            if (rows[0].text != "new" || rows[0].numbers[1] != 123456) { return 1; }
            if (copy[0].text != "old" || copy[0].numbers[1] != 8) { return 2; }
            if (view[0].text != "old" || view[0].numbers[1] != 8) { return 3; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn arrays_and_slice_views_expose_the_selected_storage() {
    parity(
        r#"
        struct Item { text:String }
        struct Group { items:[Item;2] }
        impl Item { fn change(&mut self) { self.text="changed"; } }
        fn set(x:&mut String) { *x="changed"; }
        fn read(x:&String)->String { *x }
        fn view_read(s:&Slice<Item>)->String { if (s.len() != 1) { return "wrong"; } s[0].text }
        fn array(a:&mut [Item;2]) {
            a[1].change();
            set(&mut a[a.len()-1].text);
        }
        fn main()->i16 {
            let mut g=Group{items:[Item{text:"a"},Item{text:"b"}]};
            let original=g;
            array(&mut g.items);
            if (g.items[1].text != "changed" || original.items[1].text != "b") { return 1; }
            let rows=[Item{text:"first"},Item{text:"last"}];
            let view=rows[1..2];
            if (read(&view[0].text) != "last" || view_read(&view) != "last") { return 2; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn indexed_method_receivers_evaluate_indices_once_in_order_across_calls() {
    parity(
        r#"
        struct Counter { n:i16 }
        impl Counter { fn add(&mut self, n:i16) { self.n += n; } }
        fn index(x:&mut i16, digit:i16)->usize { *x=*x*10+digit; 0 }
        fn add(x:&mut i16) { *x += 10; }
        fn main()->i16 {
            let mut order:i16=0;
            let mut grid=[[Counter{n:1}]];
            grid[index(&mut order,1)][index(&mut order,2)].add(41);
            if (order != 12 || grid[0][0].n != 42) { return 1; }
            (&mut grid[0][0]).add(1);
            if (grid[0][0].n != 43) { return 3; }
            let mut xs=[5];
            add(&mut xs[xs.len()-1]);
            if (xs[0] != 15) { return 2; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn projected_loans_end_at_scope_exit_and_reborrow_references() {
    parity(
        r#"
        struct S { n:i16 }
        fn set(x:&mut i16) { *x=42; }
        fn visit(s:&mut S) {
            { let child=&mut s.n; set(child); }
            s.n += 1;
            { let again=&mut *s; again.n += 1; }
        }
        fn main()->i16 {
            let mut s=S{n:1}; visit(&mut s);
            if (s.n != 44) { return 1; }
            let mut n:i16=0;
            let f=fn()=>{ set(&mut n); };
            f();
            if (n != 42) { return 2; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn parents_poll_and_cancel_child_future_fields_directly() {
    parity(
        r#"
        import { Future, Poll, Pending, Ready } from "std/future";
        import { Context } from "std/task";
        import { Reactor, block_on_with_budget, TurnBudgetExceeded } from "std/executor";
        struct Child { polls:i16, cancelled:bool }
        impl Future for Child {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16> {
                self.polls += 1;
                if (self.polls == 2) { Ready(42) }
                else { cx.waker.wake(); Pending }
            }
            fn cancel(&mut self) { self.cancelled=true; }
        }
        struct Parent { child:Child }
        impl Future for Parent {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16> { self.child.poll(cx) }
            fn cancel(&mut self) { self.child.cancel(); }
        }
        struct Clock { turns:u32 }
        impl Reactor for Clock {
            fn now(&self)->u32 { self.turns }
            fn turn(&mut self,wait:Option<u32>)->bool { self.turns += 1; true }
        }
        fn main()->i16 {
            let mut parent=Parent{child:Child{polls:0,cancelled:false}};
            let mut clock=Clock{turns:0};
            match block_on_with_budget(&mut parent,&mut clock,3) {
                Ok(42)=>{}, _=>{ return 1; }
            }
            if (parent.child.polls != 2 || parent.child.cancelled) { return 2; }
            let mut abandoned=Parent{child:Child{polls:0,cancelled:false}};
            match block_on_with_budget(&mut abandoned,&mut clock,0) {
                Err(TurnBudgetExceeded)=>{}, _=>{ return 3; }
            }
            if (!abandoned.child.cancelled || abandoned.child.polls != 0) { return 4; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn indexing_faults_before_evaluating_a_later_projection() {
    parity(
        r#"
        fn later()->usize { println("must not run"); 0 }
        fn read(x:&i16) { println(*x); }
        fn main() { let rows=[[1]]; read(&rows[1][later()]); }
    "#,
        1,
        "error: index out of bounds\n",
    );
}

#[test]
fn indexed_borrows_check_empty_and_full_width_bounds() {
    for declaration in ["let xs:Vec<i16> = [];", "let xs:[i16;0]=[];", "let xs=[1];"] {
        parity(&format!("fn read(x:&i16) {{ println(*x); }} fn main() {{ {declaration} read(&xs[4294967295]); }}"),
            1,"error: index out of bounds\n");
    }
    parity(
        "fn read(x:&i16) { println(*x); } fn main() { let xs=[1]; let s=xs[1..1]; read(&s[0]); }",
        1,
        "error: index out of bounds\n",
    );
}

fn rejects(source: &str, diagnostic: &str) {
    let error = rfl::check_source(source).unwrap_err().to_string();
    assert!(
        error.contains(diagnostic),
        "expected {diagnostic:?}: {error}"
    );
}

#[test]
fn projected_borrows_reject_mutation_aliasing_and_escape() {
    let prelude="struct S { n:i16, xs:Vec<i16> } fn mutate(s:&mut S)->i16 { s.n=0; 0 } fn read(n:&i16,other:i16) {} ";
    for body in [
        "let mut s=S{n:1,xs:[1]}; let r=&s.n; s.n=3;",
        "let mut s=S{n:1,xs:[1]}; let r=&s.xs[0]; s.xs.push(2);",
        "let mut s=S{n:1,xs:[1]}; let r=&mut s.n; println(s.n);",
        "let mut s=S{n:1,xs:[1]}; read(&s.n,mutate(&mut s));",
        "let mut s=S{n:1,xs:[1]}; let p=&mut s; let r=&p.xs[0]; p.xs.clear();",
        "let mut s=S{n:1,xs:[1]}; let p=&mut s; let r=&p.n; mutate(p);",
        "let mut s=S{n:1,xs:[1]}; let p=&mut s; let q=p; let r=&q.n; mutate(p);",
    ] {
        rejects(
            &format!("{prelude} fn main() {{ {body} }}"),
            "already borrowed",
        );
    }
    rejects("fn change(x:&mut Vec<i16>)->usize { x.clear(); 0 } fn main() { let mut xs=[1]; let p=&mut xs; let r=&p[change(p)]; }","already borrowed");
    rejects("fn main() { let xs=[1]; let r=&mut xs[0]; }", "not mutable");
    rejects(
        "fn main() { let xs=[1]; let mut s=xs[0..1]; let r=&mut s[0]; }",
        "read-only",
    );
    rejects(
        r#"fn main() { let s="text"; let r=&s[0]; }"#,
        "elements can be borrowed",
    );
    rejects(
        "fn make()->Vec<i16> { [1] } fn main() { let r=&make()[0]; }",
        "named place",
    );
    rejects(
        "fn main() { let xs=[1]; let p=&xs[0]; let f=fn()=>*p; }",
        "capture a reference",
    );
    rejects(
        "fn main() { let xs=[1]; let r={ &xs[0] }; }",
        "cannot escape",
    );
    rejects(
        "struct Holder { r:&i16 } fn main() {}",
        "reference cannot be stored",
    );
}

#[test]
fn reference_arguments_cannot_invalidate_a_projected_loan() {
    for call in [
        "consume(p,p);",
        "let f=fn(a:&mut Vec<i16>,b:&mut Vec<i16>)=>{}; f(p,p);",
        "generic(p,p);",
        "p.touch(p);",
    ] {
        rejects(
            &format!(
                r#"
            trait Touch {{ fn touch(&mut self, other:&mut Vec<i16>); }}
            impl Touch for Vec<i16> {{ fn touch(&mut self,other:&mut Vec<i16>) {{}} }}
            fn consume(a:&mut Vec<i16>,b:&mut Vec<i16>) {{}}
            fn generic<T>(a:&mut T,b:&mut T) {{}}
            fn main() {{ let mut xs=[1]; let p=&mut xs; {call} }}
        "#
            ),
            "already borrowed",
        );
    }
    rejects(
        "fn main() { let mut xs=[1]; let p=&mut xs; let n=&p[0]; p[0]=2; }",
        "already borrowed",
    );
    rejects(
        "fn main() { let mut xs=[1]; let p=&mut xs; let n=&p[0]; *p=[2]; }",
        "already borrowed",
    );
}

#[test]
fn projected_borrows_can_modify_nested_collection_headers_and_wide_elements() {
    parity(
        r#"
        fn grow(xs:&mut Vec<i16>) { xs.push(42); }
        fn change(n:&mut i64) { *n=1234567890123; }
        fn main()->i16 {
            let mut nested=[[1]];
            let copy=nested;
            grow(&mut nested[0]);
            if (nested[0].len() != 2 || copy[0].len() != 1) { return 1; }
            let mut wide:Vec<i64> = [1,2];
            let old=wide;
            change(&mut wide[1]);
            if (wide[1] != 1234567890123 || old[1] != 2) { return 2; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn child_future_example_runs_on_both_targets() {
    parity(
        include_str!("../examples/child-futures.rfl"),
        0,
        "hello from a child future\n125\n",
    );
}

#[test]
fn projected_borrows_convert_to_c_pointers_only_in_unsafe() {
    rejects(
        "extern \"C\" { fn use_ptr(p:*const i16); } fn main() { let xs=[1]; use_ptr(&xs[0]); }",
        "needs `unsafe`",
    );
    let program = Program::new(
        r#"
        import { change } from "./change.c";
        struct Item { value:c_int }
        fn main()->i16 {
            let mut xs=[Item{value:7}];
            let copy=xs;
            unsafe { change(&mut xs[0].value); }
            if (xs[0].value != 42 || copy[0].value != 7) { return 1; }
            42
        }
    "#,
    );
    std::fs::write(
        program.path().with_file_name("change.c"),
        "void change(int *value) { *value = 42; }",
    )
    .unwrap();
    for result in [
        rfl::run_native_file(program.path(), false).unwrap(),
        rfl::run_file(program.path()).unwrap(),
    ] {
        assert_eq!(result.status, 42, "{}", result.stdout);
        assert_eq!(result.stdout, "");
    }
}

#[test]
fn indexed_borrows_preserve_address_banks() {
    parity(
        r#"
        fn set(x:&mut i16) { *x=42; }
        fn main()->i16 {
            let mut xs:Vec<i16> = [];
            for (i in 0..70000) { xs.push(0); }
            set(&mut xs[65536]);
            if (xs[0] != 0 || xs[65536] != 42 || xs[69999] != 0) { return 1; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn forward_struct_layouts_normalize_inside_callable_and_adt_fields() {
    parity(
        r#"
        type Job<T> = Job(fn(Context)->T);
        type Boxed<T> = Boxed(T);
        struct Frame {
            job:Job<i32>, saved:Boxed<Context>,
            read:fn(&Context)->i32, transform:fn((Context,[Context;1]))->Context
        }
        struct Context { text:String, value:i32 }
        fn invoke(job:Job<i32>,cx:Context)->i32 { match job { Job(call)=>call(cx) } }
        fn main()->i16 {
            let cx=Context{text:"kept",value:123456};
            let frame=Frame{
                job:Job(fn(c:Context)=>c.value), saved:Boxed(cx),
                read:fn(c:&Context)=>c.value,
                transform:fn(pair:(Context,[Context;1]))=>{
                    let (first,rest)=pair;
                    if (rest[0].text != "kept") { return Context{text:"bad",value:0}; }
                    first
                }
            };
            if (invoke(frame.job,cx) != 123456) { return 1; }
            let read=frame.read;
            if (read(&cx) != 123456) { return 2; }
            let transform=frame.transform;
            let transformed=transform((cx,[cx]));
            if (transformed.text != "kept" || transformed.value != 123456) { return 3; }
            match frame.saved {
                Boxed(saved)=>{ if (saved.text != "kept" || saved.value != 123456) { return 4; } }
            }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn projected_field_assignments_preserve_cow_and_evaluate_indices_once() {
    parity(
        include_str!("fixtures/projected-field-assignment.rfl"),
        0,
        "projected assignment passed\n",
    );
}
