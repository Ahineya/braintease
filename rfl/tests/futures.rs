//! Polling is a library protocol; exercise the shipped modules on both backends.
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
            "rfl-futures-{}-{stamp}-{sequence}",
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
fn wakeups_coalesce_survive_poll_and_reject_stale_reused_slots() {
    parity(
        r#"
        import { task_set } from "std/task";
        fn main() -> i16 {
            let queue=match task_set(1, 0) { Ok(q)=>q, Err(_)=>{ return 1; } };
            let first=match queue.allocate() { Ok(w)=>w, Err(_)=>{ return 2; } };
            if (!first.take_ready() || first.take_ready()) { return 3; }
            if (!first.wake() || !first.wake() || !first.take_ready() || first.take_ready()) { return 4; }
            if (!first.wake() || !first.take_ready()) { return 5; }
            if (!first.retire() || first.wake() || first.retire()) { return 6; }
            let next=match queue.allocate() { Ok(w)=>w, Err(_)=>{ return 7; } };
            if (!next.take_ready() || first.wake() || next.take_ready()) { return 8; }
            match queue.allocate() { Ok(_)=>{ return 9; }, Err(_)=>{} }
            next.retire();
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn callable_return_types_supply_numeric_context_and_respect_shadowing() {
    rfl::check_source(
        r#"
        fn value() -> i16 { 0 }
        fn call(value:fn()->u32) -> bool { value() != 0 }
        fn make() -> fn()->bool {
            let value:fn()->u32=fn()=>42;
            fn()=>value() == 42
        }
        fn ccall(value:extern "C" fn()->u32) -> bool { value() > 0 }
        fn main() { let f=make(); println(f()); }
    "#,
    )
    .unwrap();
}

#[test]
fn timers_use_lazy_start_earliest_deadline_and_wrapping_monotonic_time() {
    parity(
        r#"
        import { task_set } from "std/task";
        import { Future, Pending, Ready, sleep } from "std/future";
        import { Context } from "std/task";
        fn main() -> i16 {
            let queue=match task_set(1, 4294967290) { Ok(q)=>q, Err(_)=>{ return 1; } };
            let w=match queue.allocate() { Ok(w)=>w, Err(_)=>{ return 2; } };
            w.take_ready();
            w.wake_at(9); w.wake_at(4); w.wake_at(14);
            match queue.wait_hint() { Some(10)=>{}, _=>{ return 3; } }
            if (!queue.tick(3) || w.take_ready()) { return 4; }
            if (!queue.tick(4) || !w.take_ready()) { return 5; }
            if (queue.tick(3)) { return 6; }
            match queue.wait_hint() { None=>{}, _=>{ return 7; } }
            w.retire();
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn ready_yield_and_sleep_run_with_a_deterministic_reactor_on_both_targets() {
    parity(
        r#"
        import { Reactor, block_on_with_budget } from "std/executor";
        import { Future, Poll, Pending, Ready, ready, yield_now, sleep } from "std/future";
        struct Clock { time:u32, turns:u32 }
        impl Reactor for Clock {
            fn now(&self) -> u32 { self.time }
            fn turn(&mut self, wait:Option<u32>) -> bool {
                self.turns += 1;
                match wait { Some(delay)=>{ self.time += delay; }, None=>{ self.time += 1; } }
                true
            }
        }
        fn main() -> i16 {
            let mut clock=Clock { time:100, turns:0 };
            let mut greeting=ready(["hello", "future"]);
            match block_on_with_budget(&mut greeting, &mut clock, 1) {
                Ok(text)=>println(text), Err(_)=>{ return 1; }
            }
            let mut yielded=yield_now();
            match block_on_with_budget(&mut yielded, &mut clock, 2) { Ok(_)=>{}, Err(_)=>{ return 2; } }
            let mut timer=match sleep(10) { Ok(t)=>t, Err(_)=>{ return 3; } };
            // Construction did not start its clock.
            clock.time=1000;
            match block_on_with_budget(&mut timer, &mut clock, 2) { Ok(_)=>{}, Err(_)=>{ return 4; } }
            if (clock.time != 1010 || clock.turns != 5) { return 5; }
            match sleep(2147483648) { Err(_)=>{}, Ok(_)=>{ return 6; } }
            42
        }
    "#,
        42,
        "[hello, future]\n",
    );
}

#[test]
fn cancellation_is_once_stale_wakers_are_inert_and_idle_tasks_are_not_repolled() {
    parity(
        r#"
        import { Reactor, block_on_with_budget, ReactorStopped, TurnBudgetExceeded, InvalidClock } from "std/executor";
        import { Future, Poll, Pending, poll_fn } from "std/future";
        import { Context, Waker, task_set } from "std/task";
        struct Probe { saved:Waker, polls:i16, cancelled:bool }
        impl Future for Probe {
            type Output=i16;
            fn poll(&mut self, cx:&mut Context)->Poll<i16> {
                self.saved=cx.waker; self.polls += 1; Pending
            }
            fn cancel(&mut self) { self.cancelled=true; }
        }
        struct Driver { time:u32, turns:u32, stop:bool, backwards:bool }
        impl Reactor for Driver {
            fn now(&self)->u32 { self.time }
            fn turn(&mut self, wait:Option<u32>)->bool {
                self.turns += 1;
                if (self.backwards) { self.time -= 1; } else { self.time += 1; }
                !self.stop
            }
        }
        fn main()->i16 {
            let mut polls:i16=0;
            let mut cancels:i16=0;
            let mut work=poll_fn<i16>(fn(cx:Context)=>{
                polls += 1;
                Pending
            }, fn()=>{ cancels += 1; });
            let mut driver=Driver{time:10,turns:0,stop:false,backwards:false};
            match block_on_with_budget(&mut work, &mut driver, 5) { Err(TurnBudgetExceeded)=>{}, _=>{ return 1; } }
            if (polls != 1 || cancels != 1) { return 2; }
            work.cancel();
            if (cancels != 1) { return 3; }
            let mut never=poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{ cancels += 1; });
            driver.stop=true;
            match block_on_with_budget(&mut never, &mut driver, 5) { Err(ReactorStopped)=>{}, _=>{ return 4; } }
            if (cancels != 2) { return 5; }
            let mut badclock=poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{ cancels += 1; });
            driver.stop=false; driver.backwards=true;
            match block_on_with_budget(&mut badclock, &mut driver, 5) { Err(InvalidClock)=>{}, _=>{ return 6; } }
            if (cancels != 3) { return 7; }
            let queue=match task_set(1,0) { Ok(q)=>q, Err(_)=>{ return 8; } };
            let dummy=match queue.allocate() { Ok(w)=>w, Err(_)=>{ return 9; } };
            dummy.retire();
            let mut probe=Probe{saved:dummy,polls:0,cancelled:false};
            driver.backwards=false;
            block_on_with_budget(&mut probe,&mut driver,4);
            if (probe.polls != 1 || !probe.cancelled || probe.saved.wake()) { return 10; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn two_roots_have_distinct_outputs_fair_polls_and_cleanup_only_unfinished_work() {
    parity(
        r#"
        import { Reactor, join_with_budget, TurnBudgetExceeded } from "std/executor";
        import { Poll, Pending, Ready, poll_fn } from "std/future";
        import { Context } from "std/task";
        struct Driver { ticks:u32 }
        impl Reactor for Driver {
            fn now(&self)->u32 { self.ticks }
            fn turn(&mut self, wait:Option<u32>)->bool { self.ticks += 1; true }
        }
        fn main()->i16 {
            let mut order:i16=0;
            let mut a:i16=0;
            let mut b:i16=0;
            let mut cancelled:i16=0;
            let mut first=poll_fn<String>(fn(cx:Context)=>{
                a += 1; order=order*10+1;
                if (a==2) { Ready("first") } else { cx.waker.wake(); Pending }
            },fn()=>{ cancelled += 1; });
            let mut second=poll_fn<Vec<i16>>(fn(cx:Context)=>{
                b += 1; order=order*10+2;
                if (b==2) { Ready([42]) } else { cx.waker.wake(); Pending }
            },fn()=>{ cancelled += 1; });
            let mut driver=Driver{ticks:0};
            match join_with_budget(&mut first,&mut second,&mut driver,5) {
                Ok((text, numbers))=>{ if (text != "first" || numbers[0] != 42) { return 1; } },
                Err(_)=>{ return 2; }
            }
            if (order != 1221 || a != 2 || b != 2 || cancelled != 0) { return 3; }
            let mut done=poll_fn<i16>(fn(cx:Context)=>Ready(7),fn()=>{ cancelled += 10; });
            let mut pending=poll_fn<i16>(fn(cx:Context)=>Pending,fn()=>{ cancelled += 1; });
            match join_with_budget(&mut done,&mut pending,&mut driver,3) { Err(TurnBudgetExceeded)=>{}, _=>{ return 4; } }
            if (cancelled != 1) { return 5; }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn zero_budget_cancels_before_poll_and_zero_sleep_completes_in_one_turn() {
    parity(
        r#"
        import { Reactor, block_on_with_budget, TurnBudgetExceeded } from "std/executor";
        import { Future, Poll, Ready, poll_fn, sleep } from "std/future";
        import { Context, task_set } from "std/task";
        struct Driver { turns:i16 }
        impl Reactor for Driver {
            fn now(&self)->u32 { 0 }
            fn turn(&mut self, wait:Option<u32>)->bool { self.turns += 1; true }
        }
        fn main()->i16 {
            let mut polls:i16=0;
            let mut cancelled:i16=0;
            let mut future=poll_fn<i16>(fn(cx:Context)=>{ polls += 1; Ready(42) },fn()=>{ cancelled += 1; });
            let mut driver=Driver{turns:0};
            match block_on_with_budget(&mut future,&mut driver,0) { Err(TurnBudgetExceeded)=>{}, _=>{ return 1; } }
            if (polls != 0 || cancelled != 1 || driver.turns != 0) { return 2; }
            future.cancel();
            if (cancelled != 1) { return 3; }
            let mut timer=match sleep(0) { Ok(t)=>t, Err(_)=>{ return 4; } };
            match block_on_with_budget(&mut timer,&mut driver,1) { Ok(_)=>{}, _=>{ return 5; } }
            match task_set(0,0) { Err(_)=>{}, _=>{ return 6; } }
            match task_set(1025,0) { Err(_)=>{}, _=>{ return 7; } }
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn copied_callback_futures_cannot_deliver_the_result_twice() {
    parity(
        r#"
        import { Reactor, block_on_with_budget } from "std/executor";
        import { ready } from "std/future";
        struct Driver { n:i16 }
        impl Reactor for Driver {
            fn now(&self)->u32 { 0 }
            fn turn(&mut self, wait:Option<u32>)->bool { true }
        }
        fn main() {
            let mut first=ready("once");
            let mut alias=first;
            let mut driver=Driver{n:0};
            block_on_with_budget(&mut first,&mut driver,1);
            block_on_with_budget(&mut alias,&mut driver,1);
            println("incorrect repeated completion");
        }
    "#,
        1,
        "error: future polled after completion or cancellation\n",
    );
}
