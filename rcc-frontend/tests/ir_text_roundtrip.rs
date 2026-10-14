use rcc_frontend::{emit_module, parse_module, Frontend};

#[test]
fn c_frontend_ir_round_trips_through_text() {
    let source = r#"
        struct Point { int x; int y; };
        static const int N = 3;
        extern int missing;
        volatile int sink;
        const char *greeting = "hi";
        int sum(int a, int b) {
            struct Point p;
            p.x = a;
            int *q = &p.x;
            *q = *q + b + N;
            sink = *q;
            int out;
            __asm__("ADD %0, %1, %2" : "=r"(out) : "r"(a), "r"(b) : "t0");
            return out + (int)greeting[0];
        }
        int main(void) {
            return sum(missing, 1);
        }
    "#;
    let module = Frontend::compile_to_ir(source, "round").expect("frontend lowers this program");
    let text = emit_module(&module);
    let parsed = parse_module(&text).unwrap_or_else(|err| panic!("{err}\n{text}"));
    assert_eq!(parsed, module);
    assert_eq!(emit_module(&parsed), text);
}
