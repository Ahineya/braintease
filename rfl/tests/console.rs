#[path = "support/fp.rs"]
mod support;
use support::{parity, Fixture};
const PRELUDE: &str = r#"
import {Console,ConsoleError,ConsoleBusy,ConsoleWriteError,ConsoleReadError,ConsoleLineTooLong,read_line,print as console_print,println as console_line} from "std/console";
import {RecordingConsole,recording_console} from "std/console_test";
import {Effect,run,flat_map,map_error} from "std/effect";
import {Result} from "std/result";
import {Option} from "std/option";
import {Future,Poll,Pending,Ready,yield_now} from "std/future";
import {Context,task_set} from "std/task";
import {Reactor,block_on,block_on_with_budget,TurnBudgetExceeded} from "std/executor";
struct Clock{time:u32}
impl Reactor for Clock{fn now(&self)->u32{self.time}fn turn(&mut self,w:Option<u32>)->bool{self.time+=1;true}}
fn execute<R,E,A>(p:Effect<R,E,A>,env:R)->Result<A,E>{let mut f=run(p,move(env));let mut c=Clock{time:0};block_on(&mut f,&mut c).unwrap()}
"#;
fn check(source: &str, expected: &str) {
    parity(&format!("{PRELUDE}\n{source}"), expected)
}
fn greeting_import() -> String {
    format!(
        "import {{greeting}} from \"{}\";",
        support::root()
            .join("rfl/examples/console_program.rfl")
            .display()
    )
}

#[test]
fn the_same_greeting_recipe_runs_twice_against_a_recording_service() {
    check(
        &format!(
            r#"{}
 fn main()->i16{{
 let program=greeting<RecordingConsole>();let console=recording_console(["Ada","Ripple"]);
 let unstarted=run(program,console);drop(move(unstarted));
 if(!console.output().is_empty()||console.consumed()!=0){{return 1;}}
 execute(program,console).unwrap();execute(program,console).unwrap();
 if(console.consumed()!=2){{return 2;}}
 let log=console.output();if(log.len()!=4){{return 3;}}
 for(i in 0..log.len()){{print(log[i]);}}0
 }}"#,
            greeting_import()
        ),
        "Name: Hello, Ada!\nName: Hello, Ripple!\n",
    );
}

#[test]
fn service_futures_are_lazy_and_recording_snapshots_use_cow() {
    check(
        r#"fn main()->i16{
 let mut console=recording_console(["first",""]);
 let read=console.read_line();let write=console.write_text("unused");drop(move(read));drop(move(write));
 if(console.consumed()!=0||!console.output().is_empty()){return 1;}
 let line=execute(read_line<RecordingConsole>(),console).unwrap();println(line);
 execute(console_print<RecordingConsole>("first"),console).unwrap();let snapshot=console.output();
 execute(console_line<RecordingConsole>("second"),console).unwrap();
 if(snapshot.len()!=1||snapshot[0]!="first"||console.output().len()!=2){return 2;}
 println(execute(read_line<RecordingConsole>(),console).unwrap());
 println(execute(read_line<RecordingConsole>(),console).unwrap());println("snapshots preserved");0
 }"#,
        "Some(first)\nSome()\nNone\nsnapshots preserved\n",
    );
}

#[test]
fn independent_services_keep_separate_input_and_output_state() {
    check(
        r#"fn main()->i16{
 let a=recording_console(["A"]);let b=recording_console(["B"]);
 println(execute(read_line<RecordingConsole>(),a).unwrap());println(execute(read_line<RecordingConsole>(),b).unwrap());
 execute(console_line<RecordingConsole>("a"),a).unwrap();
 if(!b.output().is_empty()||a.output().len()!=1){return 1;}0
 }"#,
        "Some(A)\nSome(B)\n",
    );
}

#[test]
fn larger_environments_can_delegate_console_capabilities() {
    check(
        &format!(
            r#"{}
 struct Application{{console:RecordingConsole,label:String}}
 impl Console for Application{{
 fn read_line(&mut self)->Async<Result<Option<String>,ConsoleError>>{{self.console.read_line()}}
 fn write_text(&mut self,text:String)->Async<Result<unit,ConsoleError>>{{let text=self.label+text;self.console.write_text(text)}}
 }}
 fn main(){{let console=recording_console(["Ada"]);execute(greeting<Application>(),Application{{console:console,label:"> "}}).unwrap();let log=console.output();for(i in 0..log.len()){{print(log[i]);}}}}
 "#,
            greeting_import()
        ),
        "> Name: > Hello, Ada!\n",
    );
}

#[test]
fn console_errors_short_circuit_and_the_environment_still_releases() {
    check(
        r#"struct Broken{owner:Owned<i16>}
 async fn bad_read()->Result<Option<String>,ConsoleError>{Err(ConsoleReadError)}
 async fn bad_write(text:String)->Result<unit,ConsoleError>{Err(ConsoleWriteError)}
 impl Console for Broken{
 fn read_line(&mut self)->Async<Result<Option<String>,ConsoleError>>{bad_read()}
 fn write_text(&mut self,text:String)->Async<Result<unit,ConsoleError>>{bad_write(text)}
 }
 fn main(){
 let p=flat_map(console_print<Broken>("prompt"),fn(_:unit)=>{println("unexpected");read_line<Broken>()});
 let p=map_error(p,fn(e:ConsoleError)=>match e{ConsoleWriteError=>"write failed",_=>"other"});
 println(execute(p,Broken{owner:owned(1,fn(n:&mut i16)=>println("closed"))}));
 }"#,
        "closed\nErr(write failed)\n",
    );
}

#[test]
fn cancelling_a_suspended_service_future_releases_its_owned_child() {
    check(
        r#"struct Delayed{marker:bool}
 async fn later()->Result<Option<String>,ConsoleError>{let owner=owned(1,fn(n:&mut i16)=>println("child closed"));yield_now().await;Ok(Some("late"))}
 async fn written(text:String)->Result<unit,ConsoleError>{Ok({})}
 impl Console for Delayed{
 fn read_line(&mut self)->Async<Result<Option<String>,ConsoleError>>{later()}
 fn write_text(&mut self,text:String)->Async<Result<unit,ConsoleError>>{written(text)}
 }
 fn main()->i16{
 let mut f=run(read_line<Delayed>(),Delayed{marker:false});let mut c=Clock{time:0};
 match block_on_with_budget(&mut f,&mut c,1){Err(TurnBudgetExceeded)=>{},_=>{return 1;}}
 f.cancel();println("cancelled");0
 }"#,
        "child closed\ncancelled\n",
    );
}

#[test]
fn terminal_input_is_nonblocking_and_cancelled_reads_release_the_guard() {
    let source = format!(
        r#"{PRELUDE}
 import {{Terminal,terminal}} from "std/console_terminal";
 fn main()->i16{{
 let console=terminal();let mut clock=Clock{{time:0}};
 for(i in 0..2){{let mut f=run(read_line<Terminal>(),console);
 match block_on_with_budget(&mut f,&mut clock,1){{Err(TurnBudgetExceeded)=>{{}},_=>{{return 1;}}}}f.cancel();}}
 println("cancelled twice");0
 }}"#
    );
    support::native_pending_input(&source, "cancelled twice\n");
    support::measured(&source, "cancelled twice\n", 10_000_000);
}

#[test]
fn overlapping_terminal_reads_return_busy_without_consuming_another_stream() {
    let source = format!(
        r#"{PRELUDE}
 import {{Terminal,terminal}} from "std/console_terminal";
 fn main()->i16{{
 let console=terminal();let mut a=run(read_line<Terminal>(),console);let mut b=run(read_line<Terminal>(),console);
 let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{{waker:w,now:0}};
 match a.poll(&mut cx){{Pending=>{{}},_=>{{return 1;}}}}
 match b.poll(&mut cx){{Ready(Err(ConsoleBusy))=>{{}},_=>{{return 2;}}}}
 a.cancel();let mut next=run(read_line<Terminal>(),console);
 match next.poll(&mut cx){{Pending=>{{}},_=>{{return 3;}}}}next.cancel();println("guards released");0
 }}"#
    );
    support::measured(&source, "guards released\n", 10_000_000);
}

#[test]
fn terminal_output_yields_and_cancellation_preserves_progress_and_releases_guard() {
    let source = format!(
        r#"
 import {{Terminal,terminal}} from "std/console_terminal";
 fn main()->i16{{
 let console=terminal();let mut a=run(console_print<Terminal>("{}"),console);
 let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{{waker:w,now:0}};
 match a.poll(&mut cx){{Pending=>{{}},_=>{{return 1;}}}}
 let mut b=run(console_print<Terminal>("unexpected"),console);
 match b.poll(&mut cx){{Ready(Err(ConsoleBusy))=>{{}},_=>{{return 2;}}}}
 a.cancel();a.cancel();execute(console_print<Terminal>("ok"),console).unwrap();0
 }}"#,
        "x".repeat(65)
    );
    check(&source, &format!("{}ok", "x".repeat(64)));
}

#[test]
fn native_terminal_handles_utf8_crlf_empty_and_final_unterminated_lines() {
    let source=format!("{PRELUDE}\n{}\nimport {{Terminal,terminal}} from \"std/console_terminal\";fn main(){{let p=greeting<Terminal>();let c=terminal();execute(p,c).unwrap();execute(p,c).unwrap();}}",greeting_import());
    for (input, expected) in [
        (
            "Ada\r\nRïpple\n",
            "Name: Hello, Ada!\nName: Hello, Rïpple!\n",
        ),
        ("Ada\rRipple\r", "Name: Hello, Ada!\nName: Hello, Ripple!\n"),
        ("\n", "Name: Hello, !\nName: Hello, stranger!\n"),
        ("Ada", "Name: Hello, Ada!\nName: Hello, stranger!\n"),
        ("", "Name: Hello, stranger!\nName: Hello, stranger!\n"),
    ] {
        support::native_input(&source, input.as_bytes(), expected);
    }
}

#[test]
fn ripple_terminal_uses_existing_mmio_input_and_preserves_crlf_across_reads() {
    let source=format!("{PRELUDE}\n{}\nimport {{Terminal,terminal}} from \"std/console_terminal\";fn main(){{let p=greeting<Terminal>();let c=terminal();execute(p,c).unwrap();execute(p,c).unwrap();}}",greeting_import());
    let f = Fixture::new(&source);
    let binary = support::binary(&f);
    let out = support::tool(
        std::process::Command::new(support::root().join("target/release/rvm"))
            .arg(binary)
            .args([
                "--input",
                "Ada\r\nRipple\n",
                "--headless",
                "--exit-code",
                "--memory",
                "16384000",
                "--timeout",
                "20",
            ]),
    );
    assert_eq!(
        String::from_utf8(out.stdout).unwrap(),
        "Name: Hello, Ada!\nName: Hello, Ripple!\n"
    );
}

#[test]
fn terminal_line_limit_accepts_the_boundary_and_rejects_overflow() {
    let source = format!(
        r#"{PRELUDE}
 import {{Terminal,terminal,MAX_LINE}} from "std/console_terminal";
 fn main()->i16{{let c=terminal();match execute(read_line<Terminal>(),c){{Ok(Some(text))=>{{if(text.len()!=MAX_LINE){{return 1;}}println("full line");}},Err(ConsoleLineTooLong)=>println("too long"),_=>{{return 2;}}}}0}}
 "#
    );
    support::native_input(
        &source,
        format!("{}\n", "a".repeat(4096)).as_bytes(),
        "full line\n",
    );
    support::native_input(
        &source,
        format!("{}\n", "a".repeat(4097)).as_bytes(),
        "too long\n",
    );
}

#[test]
fn recording_example_runs_on_native_and_ripple() {
    let source =
        std::fs::read_to_string(support::root().join("rfl/examples/console-recording.rfl"))
            .unwrap();
    // Preserve the example's relative application import in this temporary file.
    let source = source.replace(
        "\"./console_program\"",
        &format!(
            "\"{}\"",
            support::root()
                .join("rfl/examples/console_program.rfl")
                .display()
        ),
    );
    parity(&source, "Name: Hello, Ada!\nName: Hello, Ripple!\n");
}
