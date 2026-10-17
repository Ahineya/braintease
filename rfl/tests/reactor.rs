//! Host-clock adapters and bounded idle policy; fake time proves scheduling
//! decisions without relying on host timing. Execution covers both targets.
use std::path::{Path, PathBuf};

struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let directory = std::env::temp_dir().join(format!(
            "rfl-reactor-{}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
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
fn parity(source: &str) {
    let program = Program::new(source);
    for run in [
        rfl::run_native_file(program.path(), false),
        rfl::run_file(program.path()),
    ] {
        let result = run.unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(result.status, 42, "{}", result.stdout);
        assert_eq!(result.stdout, "");
    }
}

#[test]
fn elapsed_guest_and_service_work_reduce_waits_and_runnable_work_never_sleeps() {
    parity(
        r#"
        import { Reactor } from "std/executor";
        import { clocked_reactor, ReactorError, Service, ContinueAfter } from "std/reactor";
        fn main()->i16 {
            let mut clock:u32=100;
            let mut waited:u32=0;
            let mut services:i16=0;
            let read:fn()->Result<u32,ReactorError> = fn()=>Ok(clock);
            let idle:fn(u32)->Result<unit,ReactorError> = fn(ms:u32)=>{waited+=ms;clock+=ms;Ok({})};
            let service:fn(u32)->Service = fn(now:u32)=>{services+=1;clock+=2;ContinueAfter(None)};
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 1;}};
            clock+=7;
            if (!r.turn(Some(10)) || waited!=1 || services!=2 || r.now()!=112) {return 2;}
            clock+=20;
            if (!r.turn(Some(1)) || waited!=1 || services!=4 || r.now()!=136) {return 3;}
            if (!r.turn(Some(0)) || waited!=1 || services!=6 || r.now()!=140) {return 4;}
            if (!r.turn(None) || waited!=11 || services!=8 || r.now()!=154) {return 5;}
            42
        }
    "#,
    );
}

#[test]
fn maintenance_deadlines_account_for_callback_work_and_clock_wrap() {
    parity(
        r#"
        import { Reactor } from "std/executor";
        import { clocked_reactor, ReactorError, Service, ContinueAfter } from "std/reactor";
        fn main()->i16 {
            let mut clock:u32=4294967290;
            let mut waited:u32=0;
            let read:fn()->Result<u32,ReactorError> = fn()=>Ok(clock);
            let idle:fn(u32)->Result<unit,ReactorError> = fn(ms:u32)=>{waited+=ms;clock+=ms;Ok({})};
            let service:fn(u32)->Service = fn(now:u32)=>{clock+=2;ContinueAfter(Some(3))};
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 1;}};
            if (!r.turn(Some(8)) || waited!=1 || r.now()!=4294967295) {return 2;}
            if (!r.turn(Some(4)) || waited!=2 || r.now()!=4) {return 3;}
            if (!r.turn(None) || waited!=3 || r.now()!=9) {return 4;}
            42
        }
    "#,
    );
}

#[test]
fn external_service_wakes_a_pending_root_while_its_peer_completes() {
    parity(
        r#"
        import { Reactor, join_with_budget } from "std/executor";
        import { clocked_reactor, ReactorError, Service, ContinueAfter } from "std/reactor";
        import { poll_fn, ready, Pending, Ready } from "std/future";
        import { Context, Waker } from "std/task";
        fn main()->i16 {
            let mut clock:u32=0;
            let initial:Option<Waker> = None;
            let mut saved=owned(initial,fn(value:&mut Option<Waker>)=>{});
            // Stable fixture storage outlives both callbacks and the executor.
            let pointer=unsafe {let v=saved.borrow_mut();let p:*mut Option<Waker> = &mut *v;p};
            let mut delivered=false;
            let mut polls:i16=0;
            let mut cancelled:i16=0;
            let read:fn()->Result<u32,ReactorError> = fn()=>Ok(clock);
            let idle:fn(u32)->Result<unit,ReactorError> = fn(ms:u32)=>{clock+=ms;Ok({})};
            let service:fn(u32)->Service = fn(now:u32)=>{
                if (!delivered && now>=5) {
                    delivered=true;
                    unsafe {let value=__borrow_raw(pointer);match value {Some(w)=>{w.wake();},None=>{}}}
                }
                ContinueAfter(Some(5))
            };
            let mut event=poll_fn<i16>(fn(cx:Context)=>{
                polls+=1;unsafe {let value=__borrow_raw(pointer);*value=Some(cx.waker);}
                if (delivered) {Ready(42)} else {Pending}
            },fn()=>{cancelled+=1;});
            let mut greeting=ready("done first");
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 1;}};
            match join_with_budget(&mut event,&mut greeting,&mut r,4) {
                Ok((42,text))=>{if (text!="done first") {return 2;}},_=>{return 3;}
            }
            if (clock!=5 || polls!=2 || cancelled!=0) {return 4;}
            let value=saved.borrow();
            match value {Some(w)=>{if (w.wake() || w.is_live()) {return 5;}},None=>{return 6;}}
            42
        }
    "#,
    );
}

#[test]
fn stop_and_invalid_service_delays_are_terminal_without_extra_calls() {
    parity(
        r#"
        import { Reactor } from "std/executor";
        import { clocked_reactor, ReactorError, Service, ContinueAfter, StopReactor, InvalidServiceDelay } from "std/reactor";
        fn clock()->Result<u32,ReactorError> {Ok(100)}
        fn main()->i16 {
            let mut calls:i16=0;
            let idle:fn(u32)->Result<unit,ReactorError> = fn(ms:u32)=>{calls+=1;Ok({})};
            let mut turns:i16=0;
            let service:fn(u32)->Service = fn(now:u32)=>{
                turns+=1;if (turns==2) {StopReactor} else {ContinueAfter(None)}
            };
            let mut r=match clocked_reactor(clock,idle,service,3) {Ok(r)=>r,Err(_)=>{return 1;}};
            if (r.turn(None) || calls!=1 || turns!=2 || r.turn(None)) {return 2;}
            match r.error() {None=>{},_=>{return 3;}}
            let bad:fn(u32)->Service = fn(now:u32)=>ContinueAfter(Some(2147483648));
            let mut r=match clocked_reactor(clock,idle,bad,3) {Ok(r)=>r,Err(_)=>{return 4;}};
            if (r.turn(None) || calls!=1) {return 5;}
            match r.error() {Some(InvalidServiceDelay)=>{},_=>{return 6;}}
            let normal:fn(u32)->Service = fn(now:u32)=>ContinueAfter(None);
            let mut r=match clocked_reactor(clock,idle,normal,3) {Ok(r)=>r,Err(_)=>{return 7;}};
            r.stop();r.stop();
            if (r.turn(Some(0)) || calls!=1) {return 8;}
            42
        }
    "#,
    );
}

#[test]
fn clock_idle_and_capacity_errors_cancel_unfinished_tasks_and_keep_typed_outputs() {
    parity(
        r#"
        import { Reactor, ReactorStopped, block_on_with_budget } from "std/executor";
        import { clocked_reactor, ReactorError, Service, ContinueAfter, IdleError, ClockError, InvalidClockSample, InvalidIdleQuantum } from "std/reactor";
        import { poll_fn, Pending, sleep } from "std/future";
        import { Context } from "std/task";
        import { task_scope } from "std/scope";
        import { TaskCancelled } from "std/scope_state";
        fn main()->i16 {
            let mut clock:u32=100;
            let mut fail=false;
            let set_clock:fn(u32)->unit = fn(value:u32)=>{clock=value;};
            let set_fail:fn(bool)->unit = fn(value:bool)=>{fail=value;};
            let read:fn()->Result<u32,ReactorError> = fn()=>if (fail) {Err(ClockError(10))} else {Ok(clock)};
            let idle:fn(u32)->Result<unit,ReactorError> = fn(ms:u32)=>Err(IdleError(7));
            let service:fn(u32)->Service = fn(now:u32)=>ContinueAfter(None);
            for (q in [0,11]) {
                match clocked_reactor(read,idle,service,q as u32) {Err(InvalidIdleQuantum)=>{},_=>{return 1;}}
            }
            set_fail(true);
            match clocked_reactor(read,idle,service,10) {Err(ClockError(10))=>{},_=>{return 2;}}
            set_fail(false);
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 3;}};
            let mut cancelled:i16=0;
            let mut pending=poll_fn<i16>(fn(cx:Context)=>{cx.waker.wake_at(cx.now+1);Pending},fn()=>{cancelled+=1;});
            match block_on_with_budget(&mut pending,&mut r,5) {Err(ReactorStopped)=>{},_=>{return 4;}}
            if (cancelled!=1) {return 5;}
            match r.error() {Some(IdleError(7))=>{},_=>{return 6;}}
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 7;}};
            set_clock(99);
            if (r.turn(Some(0))) {return 8;}
            match r.error() {Some(InvalidClockSample)=>{},_=>{return 9;}}
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 10;}};
            set_fail(true);
            if (r.turn(Some(0))) {return 11;}
            match r.error() {Some(ClockError(10))=>{},_=>{return 12;}}
            set_fail(false);
            let mut r=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 13;}};
            let mut scope=match task_scope(1,r.now()) {Ok(s)=>move(s),Err(_)=>{return 14;}};
            let work=match sleep(25) {Ok(f)=>f,Err(_)=>{return 15;}};
            let mut handle=match scope.spawn(work) {Ok(h)=>move(h),Err(_)=>{return 16;}};
            match scope.run_with_budget(&mut r,5) {Err(ReactorStopped)=>{},_=>{return 17;}}
            if (scope.active()!=0) {return 18;}
            // A fresh reactor can collect the cancellation from the surviving join.
            let mut good=match clocked_reactor(read,idle,service,10) {Ok(r)=>r,Err(_)=>{return 19;}};
            match block_on_with_budget(&mut handle,&mut good,1) {Ok(Err(TaskCancelled))=>{},_=>{return 20;}}
            42
        }
    "#,
    );
}

#[test]
fn wrapping_time_conversion_uses_full_low_seconds_and_truncates_nanos() {
    parity(
        r#"
        import { HostTime } from "std/time";
        fn main()->i16 {
            let t=HostTime{seconds_low:4294967,seconds_high:1234,nanoseconds:296999999,resolution_ns:1000000};
            if (t.wrapping_millis()!=0) {return 1;}
            let t=HostTime{seconds_low:4294967295,seconds_high:0,nanoseconds:999999999,resolution_ns:1000000};
            if (t.wrapping_millis()!=4294967295) {return 2;}
            42
        }
    "#,
    );
}

#[test]
fn real_host_clock_and_idle_drive_timers_without_logical_ticks() {
    let program = Program::new(
        r#"
        import { host_reactor } from "std/host";
        import { monotonic_millis, realtime } from "std/time";
        import { Reactor, block_on_with_budget } from "std/executor";
        import { sleep } from "std/future";
        import { test_tick, test_idle } from "./probe.c";
        fn main()->i16 {
            let before=test_tick();
            if (before==4294967295) {return 1;}
            match realtime() {Ok(t)=>{if (t.seconds_low==0 && t.seconds_high==0) {return 2;}},_=>{return 3;}}
            if (test_idle(11)!=3 || test_idle(65535)!=3) {return 4;}
            let mut r=match host_reactor() {Ok(r)=>r,Err(_)=>{return 5;}};
            let start=r.now();
            let mut timer=match sleep(30) {Ok(t)=>t,Err(_)=>{return 6;}};
            match block_on_with_budget(&mut timer,&mut r,100) {Ok(_)=>{},_=>{return 7;}}
            if (r.now()-start<30) {return 8;}
            match monotonic_millis() {Ok(t)=>{if (t-r.now()>2147483647) {return 9;}},_=>{return 10;}}
            if (test_tick()!=before) {return 11;}
            42
        }
    "#,
    );
    std::fs::write(
        program.path().with_file_name("probe.c"),
        r#"
        #include <vm_devices.h>
        unsigned long test_tick(void) {
            VmClock time;
            if (vm_get_time(&time)!=VM_DEVICE_OK) return 0xffffffffUL;
            return time.tick;
        }
        unsigned short test_idle(unsigned short ms) {return vm_host_idle(ms);}
    "#,
    )
    .unwrap();
    for run in [
        rfl::run_native_file(program.path(), false),
        rfl::run_file(program.path()),
    ] {
        let result = run.unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(result.status, 42, "{}", result.stdout);
        assert_eq!(result.stdout, "");
    }
}
