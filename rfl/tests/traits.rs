fn run(source: &str, expected: &str) {
    let result = rfl::run_program(source).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(result.status, 0, "{}", result.stdout);
    assert_eq!(result.stdout, expected);
}
fn reject(source: &str, message: &str) {
    let error = rfl::check_source(source).expect_err("accepted invalid trait program");
    assert!(
        error.message.contains(message),
        "expected {message}: {error}"
    );
}
#[test]
fn user_traits_bounds_and_qualified_calls() {
    run(
        r#"
        trait Number { fn number(&self) -> i16; }
        struct Point { x: i16 }
        impl Number for Point { fn number(&self) -> i16 { self.x } }
        fn number<T: Number>(x: T) -> i16 { x.number() }
        fn main() { let p = Point { x: 42 }; println(number(p)); println(Number::number(&p)); }
    "#,
        "42\n42\n",
    );
}
#[test]
fn nested_standard_printing() {
    run(r#"
        fn main() {
            println(42); println(true); println('Z'); println("hello");
            let xs: Vec<Option<String>> = [None, Some("hello")];
            println(xs);
            let good: Result<Vec<Option<String>>, String> = Ok(xs);
            println(good);
            let bad: Result<Vec<Option<String>>, String> = Err("oops");
            println(bad);
            println(xs); println(xs[1]);
        }
    "#, "42\ntrue\nZ\nhello\n[None, Some(hello)]\nOk([None, Some(hello)])\nErr(oops)\n[None, Some(hello)]\nSome(hello)\n");
}
#[test]
fn bounds_win_over_concrete_inherent_methods() {
    run(
        r#"
        trait Read { fn read(&self) -> i16; }
        struct Item { value: i16 }
        impl Item { fn read(&self) -> i16 { 99 } }
        impl Read for Item { fn read(&self) -> i16 { self.value } }
        fn read<T: Read>(value: T) -> i16 { value.read() }
        fn main() { let x = Item { value: 42 }; println(x.read()); println(read(x)); }
    "#,
        "99\n42\n",
    );
}
#[test]
fn unused_generic_bodies_are_checked_against_their_contract() {
    reject(
        "fn broken<T>(value: T) -> unit { value.missing(); } fn main() {}",
        "unknown function",
    );
    reject(
        "fn broken<T: Missing>(value: T) -> unit {} fn main() {}",
        "unknown trait",
    );
    reject("struct Item { x: i16 } fn show<T>(value:T) -> unit { value.print(); } fn main() { show(1); }", "does not implement");
}

#[test]
fn spec_acceptance_program() {
    run(
        r#"
        trait Printable { fn print(&self); }
        type Option<T> = | None | Some(T);
        fn write_i16(value: i16) { __write_value(value); }
        fn write_str(value: String) { __write_value(value); }
        impl Printable for i16 { fn print(&self) { write_i16(*self); } }
        impl<T: Printable> Printable for Option<T> {
            fn print(&self) {
                match self {
                    None => write_str("None"),
                    Some(value) => { write_str("Some("); value.print(); write_str(")"); }
                }
            }
        }
        fn print<T: Printable>(value: T) { value.print(); }
        fn main() -> i16 {
            print(42); write_str("\n"); print(Some(123)); write_str("\n"); 0
        }
    "#,
        "42\nSome(123)\n",
    );
}

#[test]
fn rejects_invalid_declarations_and_signatures() {
    for (source, message) in [
        ("trait A { fn a(); }", "needs self"),
        ("trait A { fn a(&self) {} }", "signatures only"),
        (
            "trait A { fn a(&self); fn a(self); }",
            "duplicate trait method",
        ),
        ("trait A {} trait A {}", "duplicate trait"),
        (
            "trait A { fn a(&self); } impl A for i16 {}",
            "missing method",
        ),
        (
            "trait A {} impl A for i16 { fn x(&self) {} }",
            "has no method",
        ),
        (
            "trait A { fn a(&self); } impl A for i16 { fn a(self) {} }",
            "signature",
        ),
        (
            "trait A { fn a(&self, x: i16); } impl A for i16 { fn a(&self, x: u16) {} }",
            "signature",
        ),
        (
            "trait A { fn a(&self) -> i16; } impl A for i16 { fn a(&self) -> bool { true } }",
            "return type",
        ),
        ("trait A { fn a(&self, value); }", "explicit types"),
        ("trait A {} impl Unknown for i16 {}", "unknown trait"),
        ("trait A {} impl<T> A for i16 {}", "unconstrained impl"),
        (
            "trait A {} fn f<T: A + A>(x: T) -> unit {}",
            "duplicate bound",
        ),
        (
            "trait A {} export extern \"C\" fn f<T: A>(x: T) -> i16 { 0 }",
            "cannot be generic",
        ),
        ("extern \"C\" { fn f<T: Printable>(x: T); }", "expected '('"),
    ] {
        reject(source, message);
    }
}

#[test]
fn conservative_coherence_ignores_constraints_and_checks_repeated_parameters() {
    for source in [
        "trait A {} impl A for i16 {} impl A for i16 {}",
        "trait A {} impl<T> A for Option<T> {} impl A for Option<i16> {}",
        "trait A {} trait B {} impl<T: A> B for Vec<T> {} impl<T: B> B for Vec<T> {}",
        "trait A {} type Pair<T,U> = P(T,U); impl<T> A for Pair<T,T> {} impl<U> A for Pair<i16,U> {}",
    ] { reject(source, "overlapping"); }
    rfl::check_source("trait A {} type Pair<T,U> = P(T,U); impl<T> A for Pair<T,T> {} impl A for Pair<i16,bool> {} fn main() {}").unwrap();
}

#[test]
fn ambiguity_is_not_argument_overloading_and_inherent_names_win() {
    reject(
        r#"
        trait A { fn value(&self, x: i16) -> i16; }
        trait B { fn value(&self, x: bool) -> i16; }
        impl A for i16 { fn value(&self, x: i16) -> i16 { x } }
        impl B for i16 { fn value(&self, x: bool) -> i16 { 1 } }
        fn main() { let n = 0; n.value(1); }
    "#,
        "ambiguous method",
    );
    reject(
        r#"
        trait A { fn value(&self) -> i16; }
        impl A for i16 { fn value(&self) -> i16 { 1 } }
        impl i16 { fn value(&self, arg: i16) -> i16 { arg } }
        fn main() { let n = 0; n.value(); }
    "#,
        "expects 1 arguments",
    );
    reject(
        "struct Item { x:i16 } fn main() { println(Item { x:1 }); }",
        "does not implement",
    );
    reject(
        "struct Item { x:i16 } fn main() { let x: Result<i16,Item> = Ok(1); println(x); }",
        "does not implement",
    );
    reject("impl Printable for i16 { fn print(&self) {} }", "orphan");
}

#[test]
fn receivers_multiple_bounds_and_readonly_payloads_run_natively() {
    let source = r#"
        trait Set { fn set(&mut self, value: i16); }
        trait Read { fn read(&self) -> i16; }
        trait Consume { fn consume(self) -> i16; }
        struct Item { value: i16 }
        impl Set for Item { fn set(&mut self, value: i16) { self.value = value; } }
        impl Read for Item { fn read(&self) -> i16 { self.value } }
        impl Consume for Item { fn consume(self) -> i16 { self.value } }
        impl Set for i16 { fn set(&mut self, value: i16) { *self = value; } }
        fn change<T: Set + Read>(x: &mut T) -> i16 { x.set(42); x.read() }
        fn borrowed(text: &String) -> unit { text.print(); }
        fn main() -> i16 {
            let mut x = Item { value: 0 };
            println(change(&mut x));
            Set::set(&mut x, 10); Set::set(&mut x, 20);
            println(Read::read(&x)); println(x.consume());
            let mut n = 0; n.set(7); println(n);
            let text = Some("kept");
            match &text { Some(value) => borrowed(value), None => putc('?') }
            println(text); println(text);
            0
        }
    "#;
    let result = rfl::run_native_program(source).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(result.stdout, "42\n20\n20\n7\nkeptSome(kept)\nSome(kept)\n");
    reject("fn own(x:String) {} fn main() { let x=Some(\"hi\"); match &x { Some(s)=>own(s), None=>putc('?') } }", "expected String");
    reject(
        "fn main() { let x=Some(1); match &x { Some(s)=>{ *s=2; }, None=>putc('?') } }",
        "shared reference",
    );
}

struct Fixture(std::path::PathBuf);
impl Fixture {
    fn new(files: &[(&str, &str)]) -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("rfl-traits-{}-{stamp}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for (name, text) in files {
            std::fs::write(dir.join(name), text).unwrap();
        }
        Self(dir)
    }
    fn check(&self) -> Result<(), rfl::CompileError> {
        rfl::check_file(&self.0.join("main.rfl"))
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[test]
fn imported_traits_aliases_and_graph_wide_impl_discovery() {
    let fixture = Fixture::new(&[
        ("types.rfl", "export trait Read { fn read(&self) -> i16; } export struct Item { x: i16 }"),
        ("impl.rfl", "import {Read, Item} from \"./types\"; impl Read for Item { fn read(&self) -> i16 { self.x } } export fn load() {}"),
        ("main.rfl", "import {Read as View, Item} from \"./types\"; import {load} from \"./impl\"; fn main() { let n=Item{x:42}; println(n.read()); println(View::read(&n)); }"),
    ]);
    let error = fixture.check().unwrap_err();
    assert!(error.message.contains("orphan"), "{error}");
    // The impl must live with the trait or nominal type, and becomes visible
    // to all reachable users without importing an implementation symbol.
    std::fs::write(fixture.0.join("types.rfl"), "export trait Read { fn read(&self) -> i16; } export struct Item { x:i16 } impl Read for Item { fn read(&self)->i16 { self.x } }").unwrap();
    std::fs::write(fixture.0.join("impl.rfl"), "export fn load() {} ").unwrap();
    let result = rfl::run_native_file(&fixture.0.join("main.rfl"), false).unwrap();
    assert_eq!(result.stdout, "42\n42\n");
}

#[test]
fn trait_scope_does_not_leak_from_other_modules() {
    let fixture = Fixture::new(&[
        ("library.rfl", "export trait Read { fn read(&self)->i16; } export struct Item { x:i16 } impl Read for Item { fn read(&self)->i16 { self.x } }"),
        ("main.rfl", "import {Item} from \"./library\"; fn main() { let x=Item{x:42}; x.read(); }"),
    ]);
    assert!(fixture
        .check()
        .unwrap_err()
        .message
        .contains("unknown function"));
    std::fs::write(fixture.0.join("main.rfl"), "import {Printable as Show, println as line} from \"std/io\"; struct Item{x:i16} impl Show for Item { fn print(&self) { line(self.x); } } fn main() { line(Item{x:42}); }").unwrap();
    assert_eq!(
        rfl::run_native_file(&fixture.0.join("main.rfl"), false)
            .unwrap()
            .stdout,
        "42\n\n"
    );
}

#[test]
fn printable_example_and_nested_user_values() {
    let source = include_str!("../examples/traits.rfl");
    let result = rfl::run_native_program(source).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(
        result.stdout,
        "Point(3, 4)\n25\n[Some(Point(3, 4)), None]\nOk([Some(Point(3, 4))])\n"
    );
    let ir = rcc_ir::emit_module(&rfl::compile_to_ir(source).unwrap());
    assert!(
        !ir.contains("$param$"),
        "rigid parameters leaked into runtime IR"
    );
    assert!(
        !ir.contains("$bound$"),
        "unresolved trait call leaked into runtime IR"
    );
}

#[test]
fn borrowed_pattern_guards_initialize_payload_references() {
    let result = rfl::run_native_program(
        r#"
        fn main() -> i16 {
            let xs = Some("hello");
            match &xs {
                Some(s) if (*s == "wrong") => println("bad"),
                Some(s) if (*s == "hello") => s.print(),
                _ => println("bad"),
            }
            println(xs);
            0
        }
    "#,
    )
    .unwrap();
    assert_eq!(result.stdout, "helloSome(hello)\n");
    assert_eq!(result.status, 0);
}

#[test]
fn traits_on_c_layout_types_can_call_c() {
    let fixture = Fixture::new(&[
        ("math.c", "int twice(int x) { return x * 2; }"),
        (
            "main.rfl",
            r#"
            import {twice} from "./math.c";
            #[repr(C)] struct Item { value: c_int }
            trait Read { fn read(&self) -> c_int; }
            impl Read for Item { fn read(&self) -> c_int { twice(self.value) } }
            fn get<T:Read>(value:T) -> c_int { value.read() }
            fn main() -> i16 { get(Item { value:21 }) }
        "#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&fixture.0.join("main.rfl"), false)
            .unwrap()
            .status,
        42
    );
}

#[test]
fn generic_contract_checking_preserves_inferred_helper_returns() {
    let source = r#"
        trait Read { fn read(&self) -> i16; }
        struct Item { x: i16 }
        impl Read for Item { fn read(&self) -> i16 { answer() } }
        impl Item { fn read(&self) -> bool { false } }
        fn first<T>(value: T) -> i16 { second(Item{x:0}) }
        fn second<T: Read>(value: T) -> i16 { value.read() }
        fn answer() { 42 }
        fn main() -> i16 { first(0) }
    "#;
    assert_eq!(rfl::run_native_program(source).unwrap().status, 42);
}

#[test]
fn local_option_identity_does_not_take_over_the_standard_prelude() {
    let source = r#"
        type Option<T> = Empty | Present(T);
        impl<T: Printable> Printable for Option<T> {
            fn print(&self) {
                match self { Empty => print("empty"), Present(v) => v.print() }
            }
        }
        fn main() { println(Present("local")); }
    "#;
    assert_eq!(rfl::run_native_program(source).unwrap().stdout, "local\n");
}

#[test]
fn borrowed_string_receivers_keep_their_builtin_methods() {
    let source = r#"
        trait Size { fn size(&self) -> usize; }
        impl Size for String { fn size(&self) -> usize { self.len() } }
        fn main() -> i16 {
            let text = "hello";
            if (Size::size(&text) == 5 && !(&text).is_empty()) { 42 } else { 1 }
        }
    "#;
    assert_eq!(rfl::run_native_program(source).unwrap().status, 42);
}

#[test]
fn nominal_type_bounds_are_validated_when_types_are_constructed() {
    rfl::check_source("type Boxed<T: Printable> = Box(T); fn main() { let x=Box(42); }").unwrap();
    reject("type Boxed<T: Missing> = Box(T);", "unknown trait");
    reject(
        "struct Item{x:i16} type Boxed<T: Printable> = Box(T); fn main() { let x=Box(Item{x:1}); }",
        "does not implement",
    );
    reject(
        "type Boxed<T: Printable> = Box(T); fn wrap<T>(x:T) -> Boxed<T> { Box(x) }",
        "does not implement",
    );
}

#[test]
fn managed_arguments_returns_and_reordered_impl_parameters() {
    let result = rfl::run_native_program(
        r#"
        trait Format { fn format(&self, prefix: String) -> String; }
        struct Label { text: String }
        impl Format for Label { fn format(&self, prefix: String) -> String { prefix + self.text } }
        type Pair<T,U> = Pair(T,U);
        impl<A: Printable, B: Printable> Printable for Pair<B,A> {
            fn print(&self) {
                match self { Pair(left,right) => { left.print(); putc('/'); right.print(); } }
            }
        }
        fn main() {
            let value = Label { text: "hello" };
            let prefix = ">";
            println(Format::format(&value, prefix) + "!");
            println(prefix);
            println(Pair(42, "world"));
        }
    "#,
    )
    .unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(result.stdout, ">hello!\n>\n42/world\n");
}

#[test]
fn static_trait_calls_run_on_the_brainfuck_cpu() {
    use std::process::{Command, Output};
    fn command(cmd: &mut Command) -> Output {
        let result = cmd.output().unwrap_or_else(|err| panic!("{cmd:?}: {err}"));
        assert!(
            result.status.success(),
            "{cmd:?}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        result
    }
    let source = "trait Byte { fn byte(self)->i16; } impl Byte for i16 { fn byte(self)->i16 { self } } fn output<T:Byte>(value:T) { putc(value.byte()); } fn main() { output(89); }";
    let fixture = Fixture::new(&[("main.rfl", source)]);
    let root = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let tools = root.join("src/ripple-asm/target/release");
    let asm = fixture.0.join("main.asm");
    let object = fixture.0.join("main.pobj");
    let macros = fixture.0.join("main.bfm");
    let bf = fixture.0.join("main.bf");
    std::fs::write(&asm, rfl::compile_to_asm(source).unwrap()).unwrap();
    command(
        Command::new(tools.join("rasm"))
            .arg("assemble")
            .arg(&asm)
            .arg("-o")
            .arg(&object)
            .args(["--bank-size", "64000", "--max-immediate", "65535"]),
    );
    command(
        Command::new(tools.join("rlink"))
            .arg(root.join("runtime/crt0.pobj"))
            .arg(&object)
            .arg(root.join("runtime/libruntime.par"))
            .args([
                "-f",
                "macro",
                "--standalone",
                "--bf-host",
                "--memory-banks",
                "16",
                "--bank-size",
                "64000",
                "-o",
            ])
            .arg(&macros),
    );
    command(
        Command::new(root.join("src/bf-macro-expander/target/release/bfm"))
            .arg("expand")
            .arg(&macros)
            .arg("--strip-comments")
            .arg("-o")
            .arg(&bf),
    );
    let result = command(
        Command::new(root.join("target/release/rbf"))
            .arg(&bf)
            .args(["--jit", "--headless", "--timeout", "30", "--exit-code"]),
    );
    assert_eq!(String::from_utf8(result.stdout).unwrap(), "Y");
}

#[test]
fn automatic_shared_receivers_do_not_change_explicit_borrow_semantics() {
    reject(
        "struct Item{x:i16} fn main() { let mut p=Item{x:1}; let r=&p.x; p.x=2; println(*r); }",
        "only a name",
    );
    reject(
        r#"
        trait Read { fn read(&self, other:i16) -> i16; }
        impl Read for i16 { fn read(&self, other:i16)->i16 { *self } }
        struct Item{x:i16}
        fn change(p:&mut Item)->i16 { p.x=2; 0 }
        fn main() { let mut p=Item{x:1}; p.x.read(change(&mut p)); }
    "#,
        "already borrowed",
    );
}
