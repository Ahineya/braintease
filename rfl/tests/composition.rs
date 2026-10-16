//! Owned combinators and their storage transfers run identically on both targets.
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
        let dir = std::env::temp_dir().join(format!(
            "rfl-composition-{}-{stamp}-{id}",
            std::process::id()
        ));
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
#[test]
fn replacement_transfers_owners_and_extraction_skips_only_the_outer_cleanup() {
    parity(
        r#"
        import { replace } from "std/mem";
        fn main()->i16 {
            let mut drops:i16=0;
            let mut value=owned(7,fn(v:&mut i16)=>{drops+=1;});
            let previous=replace(&mut value,owned(8,fn(v:&mut i16)=>{drops+=1;}));
            if (drops!=0) {return 1;}
            let n=move(previous).into_inner();
            if (n!=7 || drops!=0) {return 2;}
            drop(move(value));
            if (drops!=1) {return 3;}
            let inner=owned(42,fn(v:&mut i16)=>{drops+=1;});
            let outer=owned(move(inner),fn(v:&mut Owned<i16>)=>{drops+=100;});
            let inner=move(outer).into_inner();
            if (drops!=1) {return 4;}
            drop(move(inner));
            if (drops!=2) {return 5;}
            42
        }
    "#,
        42,
        "",
    );
}

const PROTOCOL: &str = r#"
import { Future, Poll, Pending, Ready, poll_fn, ready, sleep } from "std/future";
import { OwnedFuture, own_future, ready_owned } from "std/future_owned";
import { join, race, timeout, First, Second, Elapsed } from "std/combinators";
import { Context, task_set } from "std/task";
import { Reactor, block_on_with_budget, TurnBudgetExceeded, ReactorStopped, InvalidClock } from "std/executor";
struct Probe { call:fn(Context)->Poll<i16>, stop:fn()->unit }
impl Future for Probe {
    type Output=i16;
    fn poll(&mut self,cx:&mut Context)->Poll<i16> {let call=self.call;call(*cx)}
    fn cancel(&mut self) {let stop=self.stop;stop();}
}
struct Clock { now_ms:u32, stop:bool, backwards:bool }
impl Reactor for Clock {
    fn now(&self)->u32 {self.now_ms}
    fn turn(&mut self,wait:Option<u32>)->bool {
        if (self.backwards) {self.now_ms-=1;}
        else {match wait {Some(delay)=>{self.now_ms+=delay;},None=>{self.now_ms+=1;}}}
        !self.stop
    }
}
"#;
fn compose(source: &str, status: i32, stdout: &str) {
    parity(&format!("{PROTOCOL}\n{source}"), status, stdout);
}

#[test]
fn replacement_handles_managed_places_cow_aliases_and_empty_values() {
    parity(
        r#"
        import { replace, take } from "std/mem";
        struct Data { text:String, numbers:Vec<i16> }
        fn exchange(p:&mut String)->String {replace(p,"new")}
        fn main()->i16 {
            let mut x="old";
            x=replace(&mut x,"discarded");
            if (x!="old") {return 1;}
            let mut data=Data{text:"before",numbers:[7,8]};
            let snapshot=data;
            if (exchange(&mut data.text)!="before" || data.text!="new" || snapshot.text!="before") {return 2;}
            if (replace(&mut data.numbers[0],42)!=7 || data.numbers[0]!=42 || snapshot.numbers[0]!=7) {return 3;}
            let mut count:i16=0;
            let mut slot:Option<Owned<String>> = Some(owned("payload",fn(x:&mut String)=>{count+=1;}));
            let old=take(&mut slot);
            if (count!=0) {return 4;}
            match move(old) {Some(value)=>{let text=move(value).into_inner();if (text!="payload") {return 5;}},None=>{return 6;}}
            drop(move(slot));
            let empty=owned({},fn(x:&mut unit)=>{count+=100;});
            move(empty).into_inner();
            if (count!=0) {return 7;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn adapter_cancels_once_and_does_not_cancel_a_completed_child() {
    compose(
        r#"
        fn main()->i16 {
            let mut calls:i16=0;let mut polls:i16=0;
            {let never=own_future(Probe{call:fn(cx:Context)=>{polls+=1;Pending},stop:fn()=>{calls+=1;}});}
            if (calls!=1 || polls!=0) {return 1;}
            let queue=match task_set(1,0) {Ok(q)=>q,Err(_)=>{return 2;}};
            let w=match queue.allocate(){Ok(w)=>w,Err(_)=>{return 3;}};
            let mut cx=Context{waker:w,now:0};
            {
                let mut pending=own_future(Probe{call:fn(cx:Context)=>{polls+=1;Pending},stop:fn()=>{calls+=1;}});
                pending.poll(&mut cx);pending.cancel();pending.cancel();
            }
            if (calls!=2 || polls!=1) {return 4;}
            {
                let mut done=own_future(Probe{call:fn(cx:Context)=>{polls+=1;Ready(42)},stop:fn()=>{calls+=100;}});
                match done.poll(&mut cx) {Ready(42)=>{},_=>{return 5;}}
                done.cancel();done.cancel();
            }
            if (calls!=2 || polls!=2) {return 6;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn ready_owned_transfers_resource_outputs_and_drops_unpolled_values() {
    compose(
        r#"
        fn main()->i16 {
            let mut drops:i16=0;
            {let never=ready_owned(owned(1,fn(x:&mut i16)=>{drops+=1;}));}
            if (drops!=1) {return 1;}
            let mut clock=Clock{now_ms:0,stop:false,backwards:false};
            let mut future=ready_owned(owned("output",fn(x:&mut String)=>{drops+=1;}));
            match block_on_with_budget(&mut future,&mut clock,1) {
                Ok(value)=>{if (*value.borrow()!="output" || drops!=1) {return 2;}},
                Err(_)=>{return 3;}
            }
            if (drops!=2) {return 4;}
            let cleanup=fn(x:&mut i16)=>{drops+=1;};
            let fresh=poll_fn<Owned<i16>>(fn(cx:Context)=>Ready(owned(42,cleanup)),fn()=>{});
            let mut future=own_future(fresh);
            match block_on_with_budget(&mut future,&mut clock,1) {
                Ok(value)=>{if (*value.borrow()!=42 || drops!=2) {return 5;}},Err(_)=>{return 6;}
            }
            if (drops!=3) {return 7;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn join_alternates_polls_and_caches_distinct_outputs() {
    compose(
        r#"
        fn main()->i16 {
            let mut order:i16=0;let mut a:i16=0;let mut b:i16=0;let mut cancels:i16=0;
            let first=poll_fn<String>(fn(cx:Context)=>{
                a+=1;order=order*10+1;
                if (a==2) {Ready("first")} else {cx.waker.wake();Pending}
            },fn()=>{cancels+=1;});
            let second=poll_fn<Vec<i16>>(fn(cx:Context)=>{
                b+=1;order=order*10+2;
                if (b==2) {Ready([42])} else {cx.waker.wake();Pending}
            },fn()=>{cancels+=1;});
            let mut both=join(first,second);
            let mut clock=Clock{now_ms:0,stop:false,backwards:false};
            match block_on_with_budget(&mut both,&mut clock,3) {
                Ok((text,values))=>{if (text!="first" || values[0]!=42) {return 1;}},Err(_)=>{return 2;}
            }
            if (order!=1221 || a!=2 || b!=2 || cancels!=0) {return 3;}
            both.cancel();
            if (cancels!=0) {return 4;}
            let mut cached_polls:i16=0;
            let cached=poll_fn<i16>(fn(cx:Context)=>{cached_polls+=1;Ready(20)},fn()=>{cancels+=100;});
            let delayed=match sleep(5) {Ok(t)=>t,Err(_)=>{return 5;}};
            let mut both=join(cached,delayed);
            match block_on_with_budget(&mut both,&mut clock,3) {Ok((20,_))=>{},_=>{return 6;}}
            if (cached_polls!=1 || cancels!=0) {return 7;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn join_cancellation_drops_cached_outputs_and_only_cancels_pending_children() {
    compose(
        r#"
        fn main()->i16 {
            let mut cancels:i16=0;let mut drops:i16=0;
            let mut clock=Clock{now_ms:0,stop:false,backwards:false};
            {
                let first=ready_owned(owned(7,fn(x:&mut i16)=>{drops+=1;}));
                let second=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
                let mut both=join(move(first),second);
                match block_on_with_budget(&mut both,&mut clock,3) {Err(TurnBudgetExceeded)=>{},_=>{return 1;}}
                if (cancels!=1 || drops!=1) {return 2;}
                both.cancel();
            }
            if (cancels!=1 || drops!=1) {return 3;}
            {
                let first=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
                let second=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
                let never=join(first,second);
            }
            if (cancels!=3) {return 4;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn race_ties_are_left_biased_and_losers_are_cancelled_before_return() {
    compose(
        r#"
        fn main()->i16 {
            let mut cancels:i16=0;let mut polls:i16=0;
            let mut clock=Clock{now_ms:0,stop:false,backwards:false};
            let left=Probe{call:fn(cx:Context)=>Ready(20),stop:fn()=>{cancels+=100;}};
            let right=Probe{call:fn(cx:Context)=>{polls+=1;Ready(22)},stop:fn()=>{cancels+=1;}};
            let mut competition=race(left,right);
            match block_on_with_budget(&mut competition,&mut clock,1) {Ok(First(20))=>{},_=>{return 1;}}
            if (cancels!=1 || polls!=0) {return 2;}
            competition.cancel();
            let left=Probe{call:fn(cx:Context)=>{polls+=1;Pending},stop:fn()=>{cancels+=1;}};
            let right=Probe{call:fn(cx:Context)=>{polls+=1;Ready(42)},stop:fn()=>{cancels+=100;}};
            let mut competition=race(left,right);
            match block_on_with_budget(&mut competition,&mut clock,1) {Ok(Second(42))=>{},_=>{return 3;}}
            if (cancels!=2 || polls!=2) {return 4;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn nested_composition_preserves_resources_and_ignores_losing_child_wakes() {
    compose(
        r#"
        fn main()->i16 {
            let mut polls:i16=0;let mut cancels:i16=0;let mut drops:i16=0;
            let queue=match task_set(1,0) {Ok(q)=>q,Err(_)=>{return 1;}};
            let w=match queue.allocate(){Ok(w)=>w,Err(_)=>{return 2;}};
            let stale=w;
            let pending=Probe{call:fn(cx:Context)=>{polls+=1;cx.waker.wake();Pending},stop:fn()=>{cancels+=1;}};
            let winner=ready_owned(owned(42,fn(x:&mut i16)=>{drops+=1;}));
            let inner=race(pending,move(winner));
            let timer=match sleep(10) {Ok(t)=>t,Err(_)=>{return 3;}};
            let mut outer=join(move(inner),timer);
            let mut cx=Context{waker:w,now:0};
            match outer.poll(&mut cx) {Pending=>{},_=>{return 4;}}
            if (polls!=1 || cancels!=1 || drops!=0) {return 5;}
            stale.wake();
            cx.now=5;
            match outer.poll(&mut cx) {Pending=>{},_=>{return 6;}}
            if (polls!=1 || cancels!=1) {return 7;}
            cx.now=10;
            match outer.poll(&mut cx) {
                Ready((Second(value),_))=>{if (*value.borrow()!=42 || drops!=0) {return 8;}},
                _=>{return 9;}
            }
            if (drops!=1 || cancels!=1) {return 10;}
            w.retire();
            if (stale.wake()) {return 11;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn timeout_is_lazy_wraps_and_gives_ready_work_the_tie() {
    compose(
        r#"
        fn main()->i16 {
            let mut cancels:i16=0;
            let mut clock=Clock{now_ms:0,stop:false,backwards:false};
            let mut work=match timeout(ready_owned(42),0) {Ok(f)=>move(f),Err(_)=>{return 1;}};
            match block_on_with_budget(&mut work,&mut clock,1) {Ok(Ok(42))=>{},_=>{return 2;}}
            let pending=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
            let mut work=match timeout(pending,0) {Ok(f)=>move(f),Err(_)=>{return 3;}};
            match block_on_with_budget(&mut work,&mut clock,1) {Ok(Err(Elapsed))=>{},_=>{return 4;}}
            if (cancels!=1) {return 5;}
            let pending=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
            let mut work=match timeout(pending,10) {Ok(f)=>move(f),Err(_)=>{return 6;}};
            clock.now_ms=4294967290;
            match block_on_with_budget(&mut work,&mut clock,2) {Ok(Err(Elapsed))=>{},_=>{return 7;}}
            if (clock.now_ms!=4 || cancels!=2) {return 8;}
            let pending=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
            match timeout(pending,2147483648) {Err(_)=>{},Ok(_)=>{return 9;}}
            if (cancels!=3) {return 10;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn runner_errors_cancel_the_whole_owned_tree_even_before_first_poll() {
    compose(
        r#"
        fn main()->i16 {
            let mut cancels:i16=0;
            let mut clock=Clock{now_ms:10,stop:false,backwards:false};
            for (i in 0..3) {
                let work=Probe{call:fn(cx:Context)=>Pending,stop:fn()=>{cancels+=1;}};
                let mut tree=match timeout(work,100) {Ok(f)=>move(f),Err(_)=>{return 1;}};
                clock.stop=i==1;clock.backwards=i==2;
                let budget:u32=if (i==0) {0} else {5};
                match block_on_with_budget(&mut tree,&mut clock,budget) {
                    Err(TurnBudgetExceeded)=>{if (i!=0) {return 2;}},
                    Err(ReactorStopped)=>{if (i!=1) {return 3;}},
                    Err(InvalidClock)=>{if (i!=2) {return 4;}},
                    _=>{return 5;}
                }
                tree.cancel();
            }
            if (cancels!=3) {return 6;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn terminal_owned_composites_report_protocol_errors() {
    for body in [
        "let mut f=ready_owned(1);f.poll(&mut cx);f.poll(&mut cx);",
        "let mut f=join(ready(1),ready(2));f.cancel();f.poll(&mut cx);",
        "let mut f=race(ready(1),ready(2));f.poll(&mut cx);f.poll(&mut cx);",
    ] {
        compose(
            &format!(
                r#"fn main() {{
            let queue=match task_set(1,0) {{Ok(q)=>q,Err(_)=>{{return;}}}};
            let w=match queue.allocate(){{Ok(w)=>w,Err(_)=>{{return;}}}};
            let mut cx=Context{{waker:w,now:0}};
            {body}
        }}"#
            ),
            1,
            "error: future polled after completion or cancellation\n",
        );
    }
}

#[test]
fn owned_composition_and_storage_transfers_reject_implicit_copies_and_bad_borrows() {
    for body in [
        "let f=ready_owned(1);let alias=f;",
        "let f=ready_owned(1);let alias=join(f,ready(2));",
        "let f=race(ready(1),ready(2));let alias=f;",
        "let mut x=owned(1,fn(x:&mut i16)=>{});let n=x.into_inner();",
        "let mut x=owned(1,fn(x:&mut i16)=>{});let r=x.borrow();let n=move(x).into_inner();",
        "let mut x=1;replace(&x,2);",
        "let mut x=1;replace(&mut x,true);",
        "let mut x=owned(1,fn(x:&mut i16)=>{});let y=move(x);replace(&mut x,owned(2,fn(x:&mut i16)=>{}));",
    ] {
        let p=Program::new(&format!("{PROTOCOL}\nimport {{replace}} from \"std/mem\";fn main() {{{body}}}"));
        assert!(rfl::check_file(&p.0).is_err(),"accepted {body}");
    }
}

#[test]
fn child_poll_addresses_stay_stable_across_parent_moves() {
    compose(
        r#"
        struct AddressProbe { address:u16, bank:u16, polls:i16 }
        impl Future for AddressProbe {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16> {
                let (address,bank)=unsafe {
                    let p:*const i16=&self.polls;
                    (p.addr(),p.bank())
                };
                if (self.polls!=0 && (address!=self.address || bank!=self.bank)) {return Ready(1);}
                self.address=address;self.bank=bank;self.polls+=1;
                if (self.polls==3) {Ready(42)} else {cx.waker.wake();Pending}
            }
            fn cancel(&mut self) {}
        }
        fn main()->i16 {
            let probe=AddressProbe{address:0,bank:0,polls:0};
            let inner=join(probe,ready_owned("cached"));
            let timer=match sleep(5) {Ok(t)=>t,Err(_)=>{return 1;}};
            let mut outer=join(move(inner),timer);
            let mut clock=Clock{now_ms:0,stop:false,backwards:false};
            match block_on_with_budget(&mut outer,&mut clock,5) {
                Ok(((42,text),_))=>{if (text!="cached") {return 2;}},
                _=>{return 3;}
            }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn extraction_reclaims_managed_environments_and_transfers_nested_containers() {
    parity(
        r#"
        import { replace } from "std/mem";
        fn main()->i16 {
            let mut calls:i16=0;
            for (i in 0..100) {
                let mut values:Vec<Owned<i16>> = [];
                values.push(owned(i,fn(n:&mut i16)=>{calls+=1;}));
                let environment:Vec<String> = ["captured", "cleanup"];
                let wrapper=owned(move(values),fn(v:&mut Vec<Owned<i16>>)=>{
                    println(environment);calls+=1000;
                });
                let mut extracted=move(wrapper).into_inner();
                if (extracted.len()!=1) {return 1;}
                let previous=replace(&mut extracted,[owned(7,fn(n:&mut i16)=>{calls+=1;})]);
                if (calls!=i*2) {return 2;}
                drop(move(previous));
                extracted.clear();
                if (calls!=i*2+2) {return 3;}
            }
            if (calls!=200) {return 4;}
            42
        }
    "#,
        42,
        "",
    );
}
