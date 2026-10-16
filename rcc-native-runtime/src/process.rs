//! Cooperative hosted-process prototype. Native steps return before scheduling;
//! neither Rust borrows nor host return addresses survive a context switch.
use super::memory::{linear, pointer};
use super::*;
use std::collections::VecDeque;

const DEFAULT_MAX_PROCESSES: usize = 256;

#[derive(Clone)]
struct Frame {
    token: u32,
    args: Vec<u64>,
    pc: u64,
    slots: BTreeMap<u64, u64>,
    base: Option<usize>,
    mark: usize,
    result: u64,
}
impl Frame {
    fn new(token: u32, args: Vec<u64>, result: u64) -> Self {
        Self {
            token,
            args,
            pc: 0,
            slots: BTreeMap::new(),
            base: None,
            mark: 0,
            result,
        }
    }
    fn result(&mut self, slot: u64, value: u64) {
        if slot != 0 {
            self.slots.insert(slot - 1, value);
        }
    }
}

#[derive(Clone)]
struct Image {
    memory: address_space::Memory,
    stack: usize,
    free: BTreeMap<usize, usize>,
    allocated: BTreeMap<usize, usize>,
    rng: u32,
}
#[derive(Clone, Copy)]
struct Wait {
    pid: u16,
    status: u64,
    result: u64,
}
struct Process {
    pid: u16,
    parent: u16,
    frames: Vec<Frame>,
    image: Option<Image>,
    wait: Option<Wait>,
}
#[derive(Clone, Copy)]
struct Request {
    op: u64,
    a: u64,
    b: u64,
    result: u64,
}

#[derive(Default)]
pub(super) struct Machine {
    active: Option<Process>,
    runnable: VecDeque<Process>,
    waiting: Vec<Process>,
    // PID -> (parent, raw exit status). Zombies count towards the process cap.
    zombies: BTreeMap<u16, (u16, u16)>,
    next_pid: u16,
    request: Option<Request>,
    arguments: Vec<u64>,
    root_status: Option<u64>,
    max_processes: usize,
}
impl Machine {
    fn frame(&mut self) -> &mut Frame {
        self.active
            .as_mut()
            .and_then(|p| p.frames.last_mut())
            .unwrap_or_else(|| fatal("native process has no active continuation"))
    }
    fn children(&self, parent: u16, pid: u16) -> bool {
        self.runnable
            .iter()
            .chain(&self.waiting)
            .any(|p| p.parent == parent && (pid == u16::MAX || pid == p.pid))
    }
    fn zombie(&self, parent: u16, pid: u16) -> Option<(u16, u16)> {
        self.zombies.iter().find_map(|(&child, &(p, status))| {
            (p == parent && (pid == u16::MAX || child == pid)).then_some((child, status))
        })
    }
}

impl Runtime {
    fn park_image(&mut self) -> Image {
        Image {
            memory: std::mem::take(&mut self.memory),
            stack: self.stack,
            free: std::mem::take(&mut self.free),
            allocated: std::mem::take(&mut self.allocated),
            rng: self.rng,
        }
    }
    fn clone_image(&self) -> Image {
        Image {
            memory: self.memory.clone(),
            stack: self.stack,
            free: self.free.clone(),
            allocated: self.allocated.clone(),
            rng: self.rng,
        }
    }
    fn restore_image(&mut self, image: Image) {
        self.memory = image.memory;
        self.stack = image.stack;
        self.free = image.free;
        self.allocated = image.allocated;
        self.rng = image.rng;
    }
    fn rotate_process(&mut self) {
        if self.processes.runnable.is_empty() {
            return;
        }
        let image = self.park_image();
        let mut process = self.processes.active.take().unwrap();
        process.image = Some(image);
        self.processes.runnable.push_back(process);
    }
    fn select_process(&mut self) -> bool {
        if self.processes.active.is_some() {
            return true;
        }
        let Some(mut process) = self.processes.runnable.pop_front() else {
            return false;
        };
        self.restore_image(process.image.take().unwrap());
        self.processes.active = Some(process);
        true
    }
    fn fork_process(&mut self, result: u64) {
        let count = 1
            + self.processes.runnable.len()
            + self.processes.waiting.len()
            + self.processes.zombies.len();
        if count >= self.processes.max_processes || self.processes.next_pid > 32767 {
            self.processes.frame().result(result, u64::from(u16::MAX));
            return;
        }
        let pid = self.processes.next_pid;
        self.processes.next_pid += 1;
        let active = self.processes.active.as_ref().unwrap();
        let mut child = Process {
            pid,
            parent: active.pid,
            frames: active.frames.clone(),
            image: Some(self.clone_image()),
            wait: None,
        };
        child.frames.last_mut().unwrap().result(result, 0);
        self.processes.frame().result(result, u64::from(pid));
        self.processes.runnable.push_back(child);
        self.rotate_process();
    }
    fn complete_wait(&mut self, wait: Wait, child: u16, status: u16) {
        if wait.status != 0 {
            // This API writes one compatibility int, not a host int or POSIX
            // bit-packed wait status. Devices are not valid status pointers.
            let at = linear(wait.status);
            if at < BANK {
                fatal("native wait status must point to RAM");
            }
            self.memory.write(at, status);
        }
        self.processes.frame().result(wait.result, u64::from(child));
        self.processes.zombies.remove(&child);
    }
    fn wait_process(&mut self, wait: Wait) {
        let pid = self.processes.active.as_ref().unwrap().pid;
        if let Some((child, status)) = self.processes.zombie(pid, wait.pid) {
            self.complete_wait(wait, child, status);
        } else if self.processes.children(pid, wait.pid) {
            let image = self.park_image();
            let mut process = self.processes.active.take().unwrap();
            process.image = Some(image);
            process.wait = Some(wait);
            self.processes.waiting.push(process);
        } else {
            self.processes
                .frame()
                .result(wait.result, u64::from(u16::MAX));
        }
    }
    fn exit_process(&mut self, status: u64) {
        let process = self.processes.active.take().unwrap();
        if process.pid == 1 {
            self.processes.root_status = Some(status);
        }
        // Orphans are allowed to finish; no zombie is retained for a dead parent.
        for child in self
            .processes
            .runnable
            .iter_mut()
            .chain(&mut self.processes.waiting)
        {
            if child.parent == process.pid {
                child.parent = 0;
            }
        }
        self.processes
            .zombies
            .retain(|_, (parent, _)| *parent != process.pid);
        if process.parent != 0 {
            self.processes
                .zombies
                .insert(process.pid, (process.parent, status as u16));
            if let Some(index) = self.processes.waiting.iter().position(|p| {
                p.pid == process.parent
                    && p.wait
                        .is_some_and(|w| w.pid == u16::MAX || w.pid == process.pid)
            }) {
                let mut parent = self.processes.waiting.remove(index);
                let wait = parent.wait.take().unwrap();
                self.restore_image(parent.image.take().unwrap());
                self.processes.active = Some(parent);
                self.complete_wait(wait, process.pid, status as u16);
                self.rotate_process();
            }
        }
    }
    fn handle_request(&mut self, request: Request) {
        match request.op {
            1 => self.fork_process(request.result),
            2 => {
                self.processes.frame().result(request.result, 0);
                self.rotate_process();
            }
            3 => self.wait_process(Wait {
                pid: request.a as u16,
                status: request.b,
                result: request.result,
            }),
            4 => self.exit_process(request.a),
            5 => {
                let pid = self.processes.active.as_ref().unwrap().pid;
                self.processes
                    .frame()
                    .result(request.result, u64::from(pid));
            }
            6 => {
                let pointer = self.memory.kernel_allocate(request.a);
                self.processes.frame().result(request.result, pointer);
            }
            7 => {
                self.memory.kernel_release(request.a);
                self.processes.frame().result(request.result, 0);
            }
            _ => fatal("invalid native process request"),
        }
    }
}

// Helpers borrow Runtime only for the duration of a synchronous generated call.
// Scheduling happens after that call AND the enclosing native step return.
macro_rules! helper {
    ($name:ident($rt:ident $(,$arg:ident)*) $body:block) => {
        #[no_mangle]
        unsafe extern "C" fn $name(ctx: *mut Runtime, $($arg: u64),*) -> u64 {
            let $rt = unsafe { &mut *ctx };
            $body
        }
    };
}
helper!(rcc_native_process_frame(rt, cells) {
    if let Some(base) = rt.processes.frame().base { return pointer(base); }
    let mark = rt.stack;
    rt.stack = mark.checked_add(usize::try_from(cells).unwrap_or_else(|_| fatal("native frame size overflow")))
        .filter(|end| *end < 5 * BANK).unwrap_or_else(|| fatal("native process stack region exhausted"));
    let frame = rt.processes.frame();
    frame.base = Some(mark);
    frame.mark = mark;
    pointer(mark)
});
helper!(rcc_native_process_slot(rt, slot) { rt.processes.frame().slots.get(&slot).copied().unwrap_or(0) });
helper!(rcc_native_process_save(rt, slot, value) { rt.processes.frame().slots.insert(slot, value); 0 });
helper!(rcc_native_process_pc(rt) { rt.processes.frame().pc });
helper!(rcc_native_process_set_pc(rt, pc) {
    let frame = rt.processes.frame();
    frame.pc = pc;
    frame.slots.clear();
    0
});
helper!(rcc_native_process_arg(rt, index) {
    rt.processes.frame().args.get(index as usize).copied().unwrap_or_else(|| fatal("native continuation argument missing"))
});
helper!(rcc_native_process_argument(rt, index, value) {
    if index == 0 { rt.processes.arguments.clear(); }
    if index as usize != rt.processes.arguments.len() { fatal("native continuation argument order mismatch"); }
    rt.processes.arguments.push(value); 0
});
helper!(rcc_native_process_call(rt, token, count, result) {
    let arity = rt.functions.get(&(token as u32)).unwrap_or_else(|| fatal("invalid native process function token")).1;
    if count != u64::from(arity) || (count != 0 && count as usize != rt.processes.arguments.len()) {
        fatal("native continuation call signature mismatch");
    }
    let args = if count == 0 { Vec::new() } else { std::mem::take(&mut rt.processes.arguments) };
    rt.processes.active.as_mut().unwrap().frames.push(Frame::new(token as u32, args, result));
    0
});
helper!(rcc_native_process_return(rt, value) {
    let frame = rt.processes.active.as_mut().unwrap().frames.pop().unwrap();
    rt.stack = frame.mark;
    if rt.processes.active.as_ref().unwrap().frames.is_empty() {
        rt.processes.request = Some(Request { op: 4, a: value, b: 0, result: 0 });
    } else { rt.processes.frame().result(frame.result, value); }
    0
});
helper!(rcc_native_process_request(rt, op, a, b, result) {
    rt.processes.request = Some(Request { op, a, b, result }); 0
});
helper!(rcc_native_process_bad_pc(_rt) { fatal("invalid native continuation program counter") });
helper!(rcc_native_process_memory(rt) { rt.enable_process_memory(); 0 });

/// # Safety
/// ctx is the live exclusively accessed runtime; its registered function tokens
/// must all name compiler-generated resumable steps with signature (ctx)->u64.
#[no_mangle]
unsafe extern "C" fn rcc_native_process_start(
    ctx: *mut Runtime,
    token: u64,
    argc: u64,
    argv: u64,
) -> u64 {
    {
        let rt = unsafe { &mut *ctx };
        rt.enable_process_memory();
        let max_processes = std::env::var("RCC_NATIVE_MAX_PROCESSES")
            .ok()
            .map(|value| {
                value
                    .parse::<usize>()
                    .ok()
                    .filter(|n| (1..=32767).contains(n))
                    .unwrap_or_else(|| {
                        fatal("RCC_NATIVE_MAX_PROCESSES must be between 1 and 32767")
                    })
            })
            .unwrap_or(DEFAULT_MAX_PROCESSES);
        let arity = rt
            .functions
            .get(&(token as u32))
            .unwrap_or_else(|| fatal("missing native process main"))
            .1;
        let args = match arity {
            0 => Vec::new(),
            2 => vec![argc, argv],
            _ => fatal("invalid native process main arity"),
        };
        rt.processes = Machine {
            active: Some(Process {
                pid: 1,
                parent: 0,
                frames: vec![Frame::new(token as u32, args, 0)],
                image: None,
                wait: None,
            }),
            next_pid: 2,
            max_processes,
            ..Machine::default()
        };
    }
    loop {
        let address = {
            let rt = unsafe { &mut *ctx };
            if !rt.select_process() {
                if !rt.processes.waiting.is_empty() {
                    fatal("native process scheduler deadlock");
                }
                return rt.processes.root_status.unwrap_or(0);
            }
            rt.poll();
            let token = rt.processes.frame().token;
            rt.functions[&token].0
        };
        // No &mut Runtime is live across this reentry into generated code.
        let step: unsafe extern "C" fn(*mut Runtime) -> u64 =
            unsafe { std::mem::transmute(address) };
        unsafe { step(ctx) };
        let rt = unsafe { &mut *ctx };
        if let Some(request) = rt.processes.request.take() {
            rt.handle_request(request);
        }
    }
}
