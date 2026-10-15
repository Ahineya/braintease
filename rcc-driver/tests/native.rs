use std::{
    path::{Path, PathBuf},
    process::Command,
};
struct Fixture(PathBuf);
static NEXT_FIXTURE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
impl Fixture {
    fn new() -> Self {
        let n = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let serial = NEXT_FIXTURE.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let p =
            std::env::temp_dir().join(format!("rcc-native-{}-{n}-{serial}", std::process::id()));
        std::fs::create_dir_all(&p).unwrap();
        Self(p)
    }
    fn compile(&self, name: &str, source: &str, ir: bool) -> PathBuf {
        let input = self.0.join(format!("{name}.c"));
        std::fs::write(&input, source).unwrap();
        let output = self.0.join(if ir {
            format!("{name}.ir")
        } else {
            name.into()
        });
        let mut c = Command::new(env!("CARGO_BIN_EXE_rcc"));
        c.args(["compile"])
            .arg(input)
            .args(["--target", "native", "-I"])
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("../runtime/include"));
        if ir {
            c.arg("--emit-ir");
        } else {
            c.arg("-o").arg(&output);
        }
        let result = c.output().unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        if ir {
            std::fs::write(&output, result.stdout).unwrap();
        }
        output
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
#[test]
fn unused_extern_globals_need_no_runtime_definition() {
    let f = Fixture::new();
    let exe = f.compile(
        "unused",
        "extern int unused; int main(void) { return 42; }",
        false,
    );
    assert_eq!(Command::new(exe).status().unwrap().code(), Some(42));
}
#[test]
fn link_native_resolves_globals_and_isolates_static_symbols() {
    let f = Fixture::new();
    let a = f.compile(
        "a",
        "static int value=20; int shared=2; int left(void) {return value;}",
        true,
    );
    let b=f.compile("b","static int value=20; extern int shared; extern int left(void); int main(void) {return left()+value+shared;}",true);
    let exe = f.0.join("linked");
    let result = Command::new(env!("CARGO_BIN_EXE_rcc"))
        .arg("link-native")
        .arg(a)
        .arg(b)
        .arg("-o")
        .arg(&exe)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(Command::new(exe).status().unwrap().code(), Some(42));
}
#[test]
fn native_variadic_calls_keep_pointers_wide_constants_and_va_copy() {
    let f = Fixture::new();
    let exe = f.compile(
        "variadic",
        r#"
        #include <stdarg.h>
        int sum(int base, ...) {
            va_list a,b; va_start(a,base); va_copy(b,a);
            int x=va_arg(a,int); long y=va_arg(a,long); int *p=va_arg(a,int *);
            if (va_arg(b,int)!=x) return 1;
            va_end(a);va_end(b); return base+x+(int)y+*p;
        }
        int main(void) {int x=2;return sum(10,10,20L,&x);}
    "#,
        false,
    );
    assert_eq!(Command::new(exe).status().unwrap().code(), Some(42));
}
#[test]
fn argc_and_file_streams_preserve_octets_positions_and_eof() {
    let f = Fixture::new();
    let input = f.0.join("input.bin");
    std::fs::write(&input, [0, 255, 65, 66]).unwrap();
    let exe = f.compile(
        "files",
        r#"
        #include <stddef.h>
        typedef struct File File;
        extern File *fopen(const char *,const char *);
        extern int fclose(File *);
        extern int fgetc(File *);
        extern int fseek(File *,long,int);
        extern long ftell(File *);
        extern int feof(File *);
        extern size_t fread(void *,size_t,size_t,File *);
        int main(int argc,char **argv){
            unsigned char bytes[4];
            if(argc!=2||argv[2]!=NULL)return 1;
            File *f=fopen(argv[1],"rb");if(!f)return 2;
            if(fgetc(f)!=0||fgetc(f)!=255||ftell(f)!=2L)return 3;
            if(fseek(f,0L,0)||fread(bytes,2UL,2UL,f)!=2UL)return 4;
            if(bytes[0]!=0||bytes[1]!=255||bytes[2]!='A'||bytes[3]!='B')return 5;
            if(fgetc(f)!=-1||!feof(f))return 6;
            if(fseek(f,0L,0)||feof(f))return 7;
            if(fclose(f))return 8;
            return 42;
        }
    "#,
        false,
    );
    let result = Command::new(exe).arg(input).output().unwrap();
    assert_eq!(
        result.status.code(),
        Some(42),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
}
#[test]
fn native_variadic_function_pointers_and_float_promotions() {
    let f = Fixture::new();
    let exe=f.compile("varptr",r#"
        #include <stdarg.h>
        int check(int n, ...){va_list a;va_start(a,n);double f=va_arg(a,double);unsigned long long u=va_arg(a,unsigned long long);va_end(a);return f==1.5 && u==0xfedcba9876543210ULL?42:1;}
        int main(void){int (*fn)(int,...)=check;float x=1.5f;return fn(2,x,0xfedcba9876543210ULL);}
    "#,false);
    assert_eq!(Command::new(exe).status().unwrap().code(), Some(42));
}

#[test]
fn file_writes_rename_remove_and_wide_cell_comparisons() {
    let f = Fixture::new();
    let exe = f.compile("file_write", r#"
        #include <stddef.h>
        #include <stdlib.h>
        #include <string.h>
        typedef struct File File;
        extern File *fopen(const char *, const char *);
        extern int fclose(File *);
        extern size_t fwrite(const void *, size_t, size_t, File *);
        extern size_t fread(void *, size_t, size_t, File *);
        extern int fseek(File *, long, int);
        extern int rename(const char *, const char *);
        extern int remove(const char *);
        int main(int argc, char **argv) {
            unsigned char high[2] = {65535U, 0}, low[2] = {1, 0};
            if (strcmp(high, low) <= 0 || strncmp(low, high, 1) >= 0 || memcmp(high, low, 1) <= 0) return 1;
            if (abs(-123) != 123 || labs(-70000L) != 70000L || llabs(-5000000000LL) != 5000000000LL) return 2;
            if (argc != 3) return 3;
            File *p = fopen(argv[1], "w+b"); if (!p) return 4;
            unsigned char data[4] = {0, 255, 65, 66}, back[4];
            if (fwrite(data, 2UL, 2UL, p) != 2UL || fseek(p, 0L, 0)) return 5;
            if (fread(back, 1UL, 4UL, p) != 4UL || memcmp(back, data, 4)) return 6;
            if (fclose(p) || rename(argv[1], argv[2]) || remove(argv[2])) return 7;
            if (remove(argv[2]) == 0 || fopen(argv[2], "rb")) return 8;
            return 42;
        }
    "#, false);
    let a = f.0.join("original.bin");
    let b = f.0.join("renamed.bin");
    let result = Command::new(exe).arg(&a).arg(&b).output().unwrap();
    assert_eq!(
        result.status.code(),
        Some(42),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert!(!a.exists() && !b.exists());
}

#[test]
fn link_native_preserves_static_callbacks_in_global_initializers() {
    let f = Fixture::new();
    let a = f.compile(
        "static_a",
        "static int value(void) {return 20;} int (*left)(void)=value;",
        true,
    );
    let b=f.compile("static_b", "static int value(void) {return 22;} extern int (*left)(void); int (*right)(void)=value; int main(void){return left()+right();}", true);
    let exe = f.0.join("callbacks");
    let result = Command::new(env!("CARGO_BIN_EXE_rcc"))
        .arg("link-native")
        .args([a, b])
        .arg("-o")
        .arg(&exe)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(Command::new(exe).status().unwrap().code(), Some(42));
}

#[test]
fn link_native_rejects_duplicate_definitions() {
    let f = Fixture::new();
    let a = f.compile("defined", "int value(int n) {return n;}", true);
    for (name, source, message) in [(
        "duplicate",
        "int value(int n){return n;} int main(void){return value(1);}",
        "multiple definitions",
    )] {
        let b = f.compile(name, source, true);
        let result = Command::new(env!("CARGO_BIN_EXE_rcc"))
            .arg("link-native")
            .arg(&a)
            .arg(b)
            .arg("-o")
            .arg(f.0.join("invalid"))
            .output()
            .unwrap();
        assert!(!result.status.success());
        assert!(
            String::from_utf8_lossy(&result.stderr).contains(message),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
}

#[test]
fn native_wall_timeout_stops_a_loop_without_memory_accesses() {
    let f = Fixture::new();
    let exe = f.compile(
        "timeout",
        "int main(void) { while (1) {} return 0; }",
        false,
    );
    let mut child = Command::new(exe)
        .env("RCC_NATIVE_TIMEOUT", "1")
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    let start = std::time::Instant::now();
    loop {
        if child.try_wait().unwrap().is_some() {
            break;
        }
        if start.elapsed().as_secs() >= 5 {
            child.kill().unwrap();
            child.wait().unwrap();
            panic!("native wall timeout did not stop the loop");
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    let result = child.wait_with_output().unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("native wall-time limit exceeded"));
}

#[test]
fn native_graphics_can_explicitly_run_headless_without_capturing() {
    let f = Fixture::new();
    let exe = f.compile(
        "headless",
        r#"
        extern void mmio_write(unsigned short, unsigned short);
        int main(void) {
            mmio_write(16, 0x0101);
            mmio_write(6, 3);
            mmio_write(8, 1);
            mmio_write(33, 0xf800);
            mmio_write(9, 1);
            return 42;
        }
    "#,
        false,
    );
    let result = Command::new(exe)
        .env("RCC_NATIVE_HEADLESS", "1")
        .env_remove("RCC_NATIVE_FRAME")
        .output()
        .unwrap();
    assert_eq!(
        result.status.code(),
        Some(42),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
}
