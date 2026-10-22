//! Execute large dispatches, signed/unsigned boundaries and C label semantics.
use std::{path::PathBuf, process::Command};

#[test]
fn large_switches_preserve_width_labels_and_resumable_execution() {
    let dir = std::env::temp_dir().join(format!(
        "rcc-switch-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir_all(&dir).unwrap();
    struct Cleanup(PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let _cleanup = Cleanup(dir.clone());
    let cases: String = (0..256)
        .map(|i| format!("case {i}: return {};\n", (i * 73 + 19) % 251))
        .collect();
    let source = format!(
        r#"
        #include <native_process.h>
        volatile unsigned visits;
        unsigned choose(unsigned x) {{ switch(x) {{ {cases} default: return 999; }} }}
        int signed_word(int x) {{ switch(x) {{
            case -32768: return 1; case -100: return 2; case -1: return 3;
            case 0: return 4; case 17: return 5; case 32767: return 6; default: return 7;
        }} }}
        int unsigned_word(unsigned x) {{ switch(x) {{
            case 0: return 1; case 1: return 2; case 32767: return 3;
            case 32768U: return 4; case 65534U: return 5; case -1: return 6; default: return 7;
        }} }}
        int wide(unsigned long long x) {{ switch(x) {{
            case 0: return 1; case 65536UL: return 2; case 0x100000000ULL: return 3;
            case 17: return 8; case 0x70000000UL: return 9;
            case 0x7fffffffffffffffULL: return 4; case 0x8000000000000000ULL: return 5;
            case 0xffffffffffffffffULL: return 6; default: return 7;
        }} }}
        int signed_long(long x) {{ switch(x) {{
            case (-2147483647L-1): return 1; case -65536L: return 2; case -1L: return 3;
            case 0L: return 4; case 65536L: return 5; case 2147483647L: return 6; default: return 7;
        }} }}
        int labels(int enter) {{
            int result = 0;
            if (enter) goto inside;
            switch (++visits) {{
                case 1: result += 10;
                case 2: result += 20; break;
                default: result = -10; break;
                case 3: result = 3; break;
                case 4: inside: result = 4; break;
                case 5: result = 5; break;
                case 6: result = 6; break; case 7: result = 7; break;
                case 8: result = 8; break; case 9: result = 9; break;
            }}
            return result;
        }}
        int main(void) {{
            RESUMABLE
            for (unsigned x=0; x<260; ++x)
                if (choose(x) != (x<256 ? (x*73U+19U)%251U : 999U)) return 1;
            if (choose(65535U)!=999) return 2;
            if (signed_word(-32768)!=1 || signed_word(-1)!=3 || signed_word(32767)!=6 || signed_word(-2)!=7) return 3;
            if (unsigned_word(32768U)!=4 || unsigned_word(65535U)!=6 || unsigned_word(2)!=7) return 4;
            if (wide(0x8000000000000000ULL)!=5 || wide(0xffffffffffffffffULL)!=6 || wide(0x100000000ULL)!=3 || wide(70000UL)!=7) return 5;
            if (signed_long(-2147483647L-1)!=1 || signed_long(-65536L)!=2 || signed_long(65536L)!=5 || signed_long(2147483647L)!=6) return 6;
            if (labels(0)!=30 || visits!=1 || labels(1)!=4 || visits!=1) return 7;
            return 0;
        }}
    "#
    );
    for (name, entry) in [("plain", ""), ("resumable", "rcc_process_yield();")] {
        let input = dir.join(format!("{name}.c"));
        let output = dir.join(name);
        std::fs::write(&input, source.replace("RESUMABLE", entry)).unwrap();
        let result = Command::new(env!("CARGO_BIN_EXE_rcc"))
            .args(["compile", "--target", "native", "-I"])
            .arg(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"))
            .arg(input)
            .arg("-o")
            .arg(&output)
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{name}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        let result = Command::new(output)
            .env("RCC_NATIVE_HEADLESS", "1")
            .env("RCC_NATIVE_NET", "off")
            .output()
            .unwrap();
        assert_eq!(
            result.status.code(),
            Some(0),
            "{name}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
}
