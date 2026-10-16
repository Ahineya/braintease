//! Task scopes own heterogeneous futures; join handles transfer typed results.
use std::path::PathBuf;
struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let id = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir =
            std::env::temp_dir().join(format!("rfl-scopes-{}-{stamp}-{id}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("main.rfl");
        std::fs::write(&path, source).unwrap();
        Self(path)
    }
}
impl Drop for Program {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn parity(source: &str, status: i32, stdout: &str) {
    let p = Program::new(source);
    for result in [
        rfl::run_native_file(&p.0, false).unwrap_or_else(|e| panic!("{e}")),
        rfl::run_file(&p.0).unwrap_or_else(|e| panic!("{e}")),
    ] {
        assert_eq!(result.status, status, "{}", result.stdout);
        assert_eq!(result.stdout, stdout);
    }
}
const SUPPORT: &str = r#"
import { Future, Poll, Pending, Ready, poll_fn, ready, sleep } from "std/future";
import { own_future, ready_owned } from "std/future_owned";
import { task_scope, TaskScope, TaskLimit, ScopeClosed } from "std/scope";
import { TaskCancelled } from "std/scope_state";
import { Reactor, block_on_with_budget, ReactorStopped, InvalidClock, TurnBudgetExceeded } from "std/executor";
import { Context, Waker, task_set } from "std/task";
struct Clock { time:u32, stop:bool, backwards:bool, turns:i16 }
impl Reactor for Clock {
    fn now(&self)->u32 {self.time}
    fn turn(&mut self,wait:Option<u32>)->bool {
        self.turns+=1;
        if (self.backwards) {self.time-=1;} else {match wait {Some(n)=>{self.time+=n;},None=>{self.time+=1;}}}
        !self.stop
    }
}
"#;
fn scopes(source: &str, status: i32, stdout: &str) {
    parity(&format!("{SUPPORT}\n{source}"), status, stdout);
}

#[test]
fn erased_owners_preserve_typed_cleanup_and_raw_views_require_explicit_unsafe() {
    parity("fn main()->i16 {let mut empty:Vec<ErasedOwner> = [];match empty.pop() {None=>{},Some(value)=>{drop(move(value));return 1;}}42}",42,"");
    parity(
        r#"
        fn main()->i16 {
            let mut count:i16=0;
            let mut owner=owned(owned("kept",fn(s:&mut String)=>{count+=1;}),fn(s:&mut Owned<String>)=>{count+=10;});
            let pointer=unsafe {let v=owner.borrow_mut();let p:*mut Owned<String> = &mut *v;p};
            let erased=__erase_owned(move(owner));
            unsafe {let value=__borrow_raw(pointer);if (*value.borrow()!="kept") {return 1;}}
            let mut entries:Vec<ErasedOwner> = [];entries.push(move(erased));
            if (count!=0) {return 2;}
            entries.clear();
            if (count!=11) {return 3;}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn heterogeneous_tasks_deliver_typed_results_after_scope_is_gone() {
    scopes(
        r#"
        fn main()->i16 {
            let mut drops:i16=0;
            let mut clock=Clock{time:0,stop:false,backwards:false,turns:0};
            let mut scope=match task_scope(2,0) {Ok(s)=>move(s),Err(_)=>{return 1;}};
            let mut text=match scope.spawn(ready("hello")) {Ok(h)=>move(h),Err(_)=>{return 2;}};
            let mut value=match scope.spawn(ready_owned(owned(42,fn(v:&mut i16)=>{drops+=1;}))) {Ok(h)=>move(h),Err(_)=>{return 3;}};
            match scope.run_with_budget(&mut clock,1) {Ok(_)=>{},_=>{return 4;}}
            if (scope.active()!=0 || drops!=0) {return 5;}
            scope.close();drop(move(scope));
            match block_on_with_budget(&mut text,&mut clock,1) {Ok(Ok(s))=>{if (s!="hello") {return 6;}},_=>{return 7;}}
            match block_on_with_budget(&mut value,&mut clock,1) {Ok(Ok(v))=>{if (*v.borrow()!=42 || drops!=0) {return 8;}},_=>{return 9;}}
            if (drops!=1) {return 10;}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn dropping_a_handle_requests_cancellation_and_reaps_before_poll_or_respawn() {
    scopes(
        r#"
        fn main()->i16 {
            let mut polls:i16=0;let mut cancels:i16=0;
            let mut scope=match task_scope(1,0) {Ok(s)=>move(s),Err(_)=>{return 1;}};
            let work=poll_fn<i16>(fn(cx:Context)=>{polls+=1;Pending},fn()=>{cancels+=1;});
            let handle=match scope.spawn(work) {Ok(h)=>move(h),Err(_)=>{return 2;}};
            drop(move(handle));
            if (scope.active()!=0 || polls!=0 || cancels!=1) {return 3;}
            let work=poll_fn<i16>(fn(cx:Context)=>{polls+=1;Pending},fn()=>{cancels+=1;});
            let mut handle=match scope.spawn(work) {Ok(h)=>move(h),Err(_)=>{return 4;}};
            let mut clock=Clock{time:0,stop:false,backwards:false,turns:0};
            match scope.turn(&mut clock) {Ok(false)=>{},_=>{return 5;}}
            handle.cancel();handle.cancel();
            let mut next=match scope.spawn(ready(42)) {Ok(h)=>move(h),Err(_)=>{return 6;}};
            if (polls!=1 || cancels!=2) {return 7;}
            match scope.run_with_budget(&mut clock,1) {Ok(_)=>{},_=>{return 8;}}
            match block_on_with_budget(&mut next,&mut clock,1) {Ok(Ok(42))=>{},_=>{return 9;}}
            let mut dependencies=match task_scope(2,clock.time) {Ok(s)=>move(s),Err(_)=>{return 10;}};
            let leaf=match dependencies.spawn(poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancels+=1;})) {Ok(h)=>move(h),Err(_)=>{return 11;}};
            let parent=match dependencies.spawn(move(leaf)) {Ok(h)=>move(h),Err(_)=>{return 12;}};
            drop(move(parent));
            if (dependencies.active()!=0 || cancels!=3) {return 13;}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn scope_drop_cancels_pending_and_never_polled_tasks_and_wakes_joiners() {
    scopes(
        r#"
        fn main()->i16 {
            let mut cancels:i16=0;
            let queue=match task_set(1,0) {Ok(q)=>q,Err(_)=>{return 1;}};
            let waiter=match queue.allocate(){Ok(w)=>w,Err(_)=>{return 2;}};
            waiter.take_ready();let mut cx=Context{waker:waiter,now:0};
            let mut scope=match task_scope(2,0) {Ok(s)=>move(s),Err(_)=>{return 3;}};
            let mut first=match scope.spawn(poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancels+=1;})) {Ok(h)=>move(h),Err(_)=>{return 4;}};
            let mut second=match scope.spawn(poll_fn<String>(fn(cx:Context)=>Pending,fn()=>{cancels+=1;})) {Ok(h)=>move(h),Err(_)=>{return 5;}};
            match first.poll(&mut cx) {Pending=>{},_=>{return 6;}}
            drop(move(scope));
            if (cancels!=2 || !waiter.take_ready()) {return 7;}
            match first.poll(&mut cx) {Ready(Err(TaskCancelled))=>{},_=>{return 8;}}
            match second.poll(&mut cx) {Ready(Err(TaskCancelled))=>{},_=>{return 9;}}
            waiter.retire();
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn capacity_failures_consume_work_and_closed_scopes_reject_spawns() {
    scopes(
        r#"
        fn main()->i16 {
            let mut cancels:i16=0;
            match task_scope(0,0) {Err(_)=>{},_=>{return 1;}}
            match task_scope(1025,0) {Err(_)=>{},_=>{return 2;}}
            let mut scope=match task_scope(1,0) {Ok(s)=>move(s),Err(_)=>{return 3;}};
            let first=match scope.spawn(poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancels+=1;})) {Ok(h)=>move(h),Err(_)=>{return 4;}};
            match scope.spawn(poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancels+=1;})) {Err(TaskLimit)=>{},_=>{return 5;}}
            if (cancels!=1) {return 6;}
            scope.close();scope.close();
            if (cancels!=2) {return 7;}
            match scope.spawn(poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancels+=1;})) {Err(ScopeClosed)=>{},_=>{return 8;}}
            if (cancels!=3) {return 9;}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn runnable_tasks_rotate_order_idle_tasks_wait_and_join_handles_compose() {
    scopes(
        r#"
        fn main()->i16 {
            let mut order:i16=0;let mut a:i16=0;let mut b:i16=0;
            let mut scope=match task_scope(3,0) {Ok(s)=>move(s),Err(_)=>{return 1;}};
            let left=poll_fn<i16>(fn(cx:Context)=>{a+=1;order=order*10+1;if (a==2) {Ready(42)} else {cx.waker.wake();Pending}},fn()=>{});
            let right=poll_fn<String>(fn(cx:Context)=>{b+=1;order=order*10+2;if (b==2) {Ready("done")} else {cx.waker.wake();Pending}},fn()=>{});
            let left=match scope.spawn(left) {Ok(h)=>move(h),Err(_)=>{return 2;}};
            let mut right=match scope.spawn(right) {Ok(h)=>move(h),Err(_)=>{return 3;}};
            // A join handle is itself a future, with its own independently scheduled waker.
            let mut waiter=match scope.spawn(move(left)) {Ok(h)=>move(h),Err(_)=>{return 4;}};
            let mut clock=Clock{time:0,stop:false,backwards:false,turns:0};
            match scope.run_with_budget(&mut clock,4) {Ok(_)=>{},_=>{return 5;}}
            if (order!=1221 || a!=2 || b!=2) {return 6;}
            match block_on_with_budget(&mut waiter,&mut clock,1) {Ok(Ok(Ok(42)))=>{},_=>{return 7;}}
            match block_on_with_budget(&mut right,&mut clock,1) {Ok(Ok(s))=>{if (s!="done") {return 8;}},_=>{return 9;}}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn retired_generations_and_copied_dispatchers_cannot_touch_reused_storage() {
    scopes(
        r#"
        fn main()->i16 {
            let mut scope=match task_scope(1,0) {Ok(s)=>move(s),Err(_)=>{return 1;}};
            let mut old=match scope.spawn(poll_fn<Waker>(fn(cx:Context)=>Ready(cx.waker),fn()=>{})) {Ok(h)=>move(h),Err(_)=>{return 2;}};
            let dispatch={let data=scope.state.borrow();match &data.entries[0] {Some(entry)=>entry.run,None=>{return 3;}}};
            let mut clock=Clock{time:0,stop:false,backwards:false,turns:0};
            scope.run_with_budget(&mut clock,1);
            let stale=match block_on_with_budget(&mut old,&mut clock,1) {Ok(Ok(w))=>w,_=>{return 4;}};
            let mut next=match scope.spawn(ready(42)) {Ok(h)=>move(h),Err(_)=>{return 5;}};
            if (stale.wake() || stale.retire()) {return 6;}
            // The copied callback outlives its freed typed capsule. It checks a
            // separate live gate before looking at either raw payload pointer.
            if (!dispatch(Context{waker:stale,now:0})) {return 7;}
            scope.run_with_budget(&mut clock,1);
            drop(move(scope));
            if (!dispatch(Context{waker:stale,now:0})) {return 8;}
            match block_on_with_budget(&mut next,&mut clock,1) {Ok(Ok(42))=>{},_=>{return 9;}}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn timeouts_runner_errors_and_resource_outputs_cleanup_across_scope_boundaries() {
    scopes(
        r#"
        import { join, timeout, Elapsed } from "std/combinators";
        fn main()->i16 {
            let mut cancelled:i16=0;let mut drops:i16=0;
            let mut clock=Clock{time:10,stop:false,backwards:false,turns:0};
            for (i in 0..3) {
                let mut scope=match task_scope(1,clock.time) {Ok(s)=>move(s),Err(_)=>{return 1;}};
                let mut h=match scope.spawn(poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancelled+=1;})) {Ok(h)=>move(h),Err(_)=>{return 2;}};
                clock.stop=i==1;clock.backwards=i==2;
                let budget:u32=if (i==0) {0} else {5};
                match scope.run_with_budget(&mut clock,budget) {
                    Err(TurnBudgetExceeded)=>{if (i!=0) {return 3;}},
                    Err(ReactorStopped)=>{if (i!=1) {return 4;}},
                    Err(InvalidClock)=>{if (i!=2) {return 5;}},_=>{return 6;}
                }
                clock.stop=false;clock.backwards=false;
                match block_on_with_budget(&mut h,&mut clock,1) {Ok(Err(TaskCancelled))=>{},_=>{return 7;}}
            }
            if (cancelled!=3) {return 8;}
            let mut scope=match task_scope(1,clock.time) {Ok(s)=>move(s),Err(_)=>{return 9;}};
            let pending=poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{cancelled+=1;});
            let timed=match timeout(pending,5) {Ok(f)=>move(f),Err(_)=>{return 10;}};
            let work=join(ready_owned(owned(42,fn(x:&mut i16)=>{drops+=1;})),move(timed));
            let mut handle=match scope.spawn(move(work)) {Ok(h)=>move(h),Err(_)=>{return 11;}};
            scope.run_with_budget(&mut clock,3);
            match block_on_with_budget(&mut handle,&mut clock,1) {
                Ok(Ok((value,Err(Elapsed))))=>{if (*value.borrow()!=42 || drops!=0) {return 12;}},_=>{return 13;}
            }
            if (cancelled!=4 || drops!=1) {return 14;}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn pending_joiners_receive_completion_wakes_and_slots_reuse_repeatedly() {
    scopes(
        r#"
        fn main()->i16 {
            let mut drops:i16=0;
            let mut scope=match task_scope(1,0) {Ok(s)=>move(s),Err(_)=>{return 1;}};
            let queue=match task_set(1,0) {Ok(q)=>q,Err(_)=>{return 2;}};
            let w=match queue.allocate(){Ok(w)=>w,Err(_)=>{return 3;}};
            let mut cx=Context{waker:w,now:0};
            let mut clock=Clock{time:0,stop:false,backwards:false,turns:0};
            for (i in 0..30) {
                let mut h=match scope.spawn(ready_owned(owned(i,fn(v:&mut i16)=>{drops+=1;}))) {Ok(h)=>move(h),Err(_)=>{return 4;}};
                w.take_ready();
                match h.poll(&mut cx) {Pending=>{},_=>{return 5;}}
                match scope.turn(&mut clock) {Ok(true)=>{},_=>{return 6;}}
                if (!w.take_ready()) {return 7;}
                match h.poll(&mut cx) {Ready(Ok(value))=>{if (*value.borrow()!=i) {return 8;}},_=>{return 9;}}
            }
            if (drops!=30 || scope.active()!=0) {return 10;}
            w.retire();42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn scope_and_handles_are_move_only_and_erasure_does_not_expose_the_payload() {
    for body in [
        "let scope=match task_scope(1,0){Ok(s)=>move(s),Err(_)=>{return;}};let alias=scope;",
        "let mut scope=match task_scope(1,0){Ok(s)=>move(s),Err(_)=>{return;}};let h=scope.spawn(ready(1));let alias=h;",
        "let x=owned(1,fn(x:&mut i16)=>{});let e=__erase_owned(x);",
        "let x=owned(1,fn(x:&mut i16)=>{});let e=__erase_owned(move(x));e.borrow();",
        "let x=owned(1,fn(x:&mut i16)=>{});let e=__erase_owned(move(x));let alias=e;",
        "let mut x=1;let p=unsafe {let p:*mut i16=&mut x;p};let r=__borrow_raw(p);",
        "let x=1;let p=unsafe {let p:*const i16=&x;p};unsafe {__borrow_raw(p);}",
    ] {
        let program=Program::new(&format!("{SUPPORT}\nfn main() {{{body}}}"));
        assert!(rfl::check_file(&program.0).is_err(),"accepted {body}");
    }
}

#[test]
fn dropping_cached_results_cannot_retire_a_new_occupant_of_the_slot() {
    scopes(
        r#"
        fn main()->i16 {
            let mut drops:i16=0;let mut polls:i16=0;let mut cancels:i16=0;
            let mut scope=match task_scope(1,0) {Ok(s)=>move(s),Err(_)=>{return 1;}};
            let first=match scope.spawn(ready_owned(owned(1,fn(x:&mut i16)=>{drops+=1;}))) {Ok(h)=>move(h),Err(_)=>{return 2;}};
            let mut clock=Clock{time:0,stop:false,backwards:false,turns:0};
            scope.run_with_budget(&mut clock,1);
            let mut second=match scope.spawn(poll_fn<i16>(fn(cx:Context)=>{polls+=1;Pending},fn()=>{cancels+=1;})) {Ok(h)=>move(h),Err(_)=>{return 3;}};
            drop(move(first));
            if (drops!=1) {return 4;}
            scope.turn(&mut clock);
            scope.turn(&mut clock);
            if (polls!=1 || cancels!=0 || scope.active()!=1) {return 5;}
            scope.close();drop(move(scope));
            if (cancels!=1) {return 6;}
            match block_on_with_budget(&mut second,&mut clock,1) {Ok(Err(TaskCancelled))=>{},_=>{return 7;}}
            42
        }
    "#,
        42,
        "",
    );
}
#[test]
fn copied_join_callbacks_check_lifetime_and_cannot_return_an_output_twice() {
    for body in [
        "let callback=match move(handle) {HandleLive(owner,poll)=>{drop(move(owner));poll},HandleDone=>{return;}};callback(cx);",
        "let callback=match &handle {HandleLive(_,poll)=>*poll,HandleDone=>{return;}};callback(cx);callback(cx);",
    ] {
        scopes(&format!(r#"
            import {{ HandleLive, HandleDone }} from "std/scope_state";
            fn main() {{
                let mut scope=match task_scope(1,0) {{Ok(s)=>move(s),Err(_)=>{{return;}}}};
                let handle=match scope.spawn(ready(42)) {{Ok(h)=>move(h),Err(_)=>{{return;}}}};
                let mut clock=Clock{{time:0,stop:false,backwards:false,turns:0}};
                scope.run_with_budget(&mut clock,1);
                let queue=match task_set(1,0) {{Ok(q)=>q,Err(_)=>{{return;}}}};
                let w=match queue.allocate(){{Ok(w)=>w,Err(_)=>{{return;}}}};
                let cx=Context{{waker:w,now:0}};
                {body}
            }}
        "#),1,"error: future polled after completion or cancellation\n");
    }
}
