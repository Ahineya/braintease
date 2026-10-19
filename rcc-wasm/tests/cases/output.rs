use super::*;
#[test]
fn printing_twenty_thousand_octets_uses_three_host_crossings() {
    let module = c(r#"
        extern int putchar(int);
        int main(void){long i;for(i=0;i<20000L;i++)putchar('a');putchar(10);return 0;}
    "#);
    let artifact = compile(&[module], &Options::default()).unwrap();
    let path = std::env::temp_dir().join(format!("rcc-wasm-batches-{}.wasm", std::process::id()));
    fs::write(&path, artifact.bytes).unwrap();
    let script = r#"
const fs=require('node:fs');const module=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
let instance,calls=0,bytes=0;
instance=new WebAssembly.Instance(module,{ripple:{write(address,count){
 ++calls;const view=new DataView(instance.exports.memory.buffer,address,count*2);
 for(let i=0;i<count;i++) {
  const value=view.getUint16(i*2,true);const expected=bytes===20000?10:97;
  if(value!==expected)throw Error(`bad output at ${bytes}: ${value}`);++bytes;
 }
}}});
if(instance.exports.main()!==0n)throw Error('bad status');instance.exports.__ripple_flush();
if(calls!==3 || bytes!==20001)throw Error(`${calls} crossings for ${bytes} bytes`);
"#;
    let result = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", script])
        .arg(&path)
        .output()
        .unwrap();
    fs::remove_file(path).unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
}
