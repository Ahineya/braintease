use rfl::run_wasm_program;
#[test]
fn managed_collections_traits_closures_and_unicode_print_in_wasm() {
    let source = r#"
        struct Label { text: String }
        impl Printable for Label { fn print(&self) { self.text.print(); } }
        fn main() -> i16 {
            let xs:Vec<Option<String>>=[None,Some("héllo 🌍")];
            println(xs);
            let prefix="Ripple: ";
            let show=fn(value:String)=>prefix+value;
            println(Label{text:show("Wasm")});
            println(xs[1]);
            0
        }
    "#;
    let result = run_wasm_program(source).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(
        result.stdout,
        "[None, Some(héllo 🌍)]\nRipple: Wasm\nSome(héllo 🌍)\n"
    );
}
#[test]
fn comprehensions_and_owned_drop_callbacks_execute_inside_wasm() {
    let source = r#"
        fn main(){
            println(for{a<-Some(20);b<-Some(22);yield a+b});
            println(for{a<-[1,2];b<-[3,4];yield a*10+b});
            let mut dropped:i16=0;
            let owner=owned(42,fn(n:&mut i16)=>{dropped+=1;});
            println(*owner.borrow());drop(move(owner));println(dropped);
        }
    "#;
    let result = run_wasm_program(source).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(result.stdout, "Some(42)\n[13, 14, 23, 24]\n42\n1\n");
}
#[test]
fn typed_floats_and_portable_trigonometry_stay_in_wasm() {
    let source = r#"
        import {sin,cos,floor,sqrt} from "std/math";
        fn main()->i16{
            if (floor(-1.5)!=-2.0 || sqrt(9.0)!=3.0) {return 1;}
            if (sin(0.5)<0.4794255385 || sin(0.5)>0.4794255387) {return 2;}
            if (cos(1.0)<0.5403023057 || cos(1.0)>0.5403023060) {return 3;}
            println("float ok");0
        }
    "#;
    let fixture = Source::new(source);
    let result = rfl::run_wasm_file(&fixture.0, false).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(result.stdout, "float ok\n");
    let module = rfl::compile_file_for_target(&fixture.0, rfl::Target::Wasm).unwrap();
    let artifact = rcc_wasm::compile(&[module], &rcc_wasm::Options::default()).unwrap();
    // The only permitted boundary for this program is buffered text output.
    let mut imports = Vec::new();
    for payload in wasmparser::Parser::new(0).parse_all(&artifact.bytes) {
        if let wasmparser::Payload::ImportSection(reader) = payload.unwrap() {
            for import in reader {
                let import = import.unwrap();
                imports.push((import.module.to_string(), import.name.to_string()));
            }
        }
    }
    assert_eq!(imports, vec![("ripple".to_string(), "write".to_string())]);
}

#[test]
fn imported_c_and_rfl_share_the_wasm_calling_and_data_abi() {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory =
        std::env::temp_dir().join(format!("rfl-wasm-import-{}-{stamp}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    struct Fixture(std::path::PathBuf);
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let _fixture = Fixture(directory.clone());
    std::fs::write(
        directory.join("math.c"),
        "struct pair {int x;int y;};int sum(struct pair *p){return p->x+p->y;}",
    )
    .unwrap();
    std::fs::write(
        directory.join("main.rfl"),
        r#"
        import {} from "./math.c";
        #[repr(C)] struct Pair { x:c_int, y:c_int }
        extern "C" { fn sum(p:*const Pair)->c_int; }
        fn main()->i16 { let p=Pair{x:20,y:22};unsafe {println(sum(&p));}0 }
    "#,
    )
    .unwrap();
    let result = rfl::run_wasm_file(&directory.join("main.rfl"), false).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(result.stdout, "42\n");
}

struct Source(std::path::PathBuf);
impl Source {
    fn new(source: &str) -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "rfl-wasm-source-{}-{stamp}.rfl",
            std::process::id()
        ));
        std::fs::write(&path, source).unwrap();
        Self(path)
    }
}
impl Drop for Source {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}
