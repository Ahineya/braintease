//! Cooperative hosted-process prototype. Native steps return before scheduling;
//! neither Rust borrows nor host return addresses survive a context switch.
use super::memory::{linear, pointer};
use super::*;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

static STOPPING: AtomicBool = AtomicBool::new(false);
extern "C" fn stop_session(_signal: libc::c_int) {
    STOPPING.store(true, Ordering::Relaxed);
}

const DEFAULT_MAX_PROCESSES: usize = 256;

#[derive(Clone)]
struct Frame {
    id: u64,
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
            id: 0,
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
struct Jump {
    frame: Frame,
    depth: usize,
    stack: usize,
    result: u64,
    address: usize,
}

#[derive(Clone)]
struct Image {
    memory: address_space::Memory,
    stack: usize,
    free: BTreeMap<usize, usize>,
    allocated: BTreeMap<usize, usize>,
    rng: u32,
    errno: u64,
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
    module: Option<std::sync::Arc<images::Loaded>>,
    staged: Option<(u32, std::sync::Arc<images::Loaded>)>,
    jumps: BTreeMap<u64, Jump>,
    loading: Option<(usize, Vec<u8>)>,
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
    checkpoint: u32,
    checkpoint_count: u16,
    checkpoint_time: Option<Instant>,
    pub(super) hosted: bool,
    global_template: Vec<u16>,
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
    next_frame: u64,
    next_jump: u64,
}
impl Machine {
    pub(super) fn diagnostic(&self) -> String {
        self.active
            .as_ref()
            .map(|p| {
                format!(
                    "; continuations {:?}",
                    p.frames
                        .iter()
                        .rev()
                        .take(8)
                        .map(|f| (format!("{:#x}", f.token), f.pc))
                        .collect::<Vec<_>>()
                )
            })
            .unwrap_or_default()
    }
    pub(super) fn loading(&mut self) -> &mut Option<(usize, Vec<u8>)> {
        &mut self.active.as_mut().unwrap().loading
    }
    pub(super) fn owns_image(&self, image: &std::sync::Arc<images::Loaded>) -> bool {
        self.active
            .as_ref()
            .and_then(|p| p.module.as_ref())
            .is_some_and(|m| std::sync::Arc::ptr_eq(m, image))
    }
    pub(super) fn is_prelinked(&self) -> bool {
        self.active.as_ref().is_some_and(|p| p.module.is_none())
    }
    pub(super) fn current_token(&self) -> u32 {
        self.active
            .as_ref()
            .and_then(|p| p.frames.last())
            .unwrap_or_else(|| fatal("no active image frame"))
            .token
    }
    pub(super) fn stage_image(&mut self, entry: u32, image: std::sync::Arc<images::Loaded>) {
        self.active.as_mut().unwrap().staged = Some((entry, image));
    }
    pub(super) fn discard_staged(&mut self, entry: u32) {
        let process = self.active.as_mut().unwrap();
        if process
            .staged
            .as_ref()
            .is_some_and(|(token, _)| *token == entry)
        {
            process.staged = None;
        }
    }
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
            errno: self.errno,
        }
    }
    fn clone_image(&self) -> Image {
        Image {
            memory: self.memory.clone(),
            stack: self.stack,
            free: self.free.clone(),
            allocated: self.allocated.clone(),
            rng: self.rng,
            errno: self.errno,
        }
    }
    fn restore_image(&mut self, image: Image) {
        self.memory = image.memory;
        self.stack = image.stack;
        self.free = image.free;
        self.allocated = image.allocated;
        self.rng = image.rng;
        self.errno = image.errno;
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
        if self.processes.hosted {
            return false;
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
            module: active.module.clone(),
            staged: None,
            jumps: active.jumps.clone(),
            loading: None,
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
        // setjmp captures the caller's continuation, never a Rust/host stack.
        // Memory is deliberately not rolled back: volatile locals and changes
        // to globals/heap retain their values, just as for a machine setjmp.
        if request.op == 13 {
            let address = linear(request.a);
            if address < BANK || address + 4 > address_space::KERNEL_START {
                fatal("invalid native jump buffer");
            }
            self.processes.next_jump = self
                .processes
                .next_jump
                .checked_add(1)
                .unwrap_or_else(|| fatal("native jump token space exhausted"));
            let cookie = self.processes.next_jump;
            let process = self.processes.active.as_mut().unwrap();
            let frame = process.frames.last_mut().unwrap();
            frame.result(request.result, 0);
            let jump = Jump {
                frame: frame.clone(),
                depth: process.frames.len(),
                stack: self.stack,
                result: request.result,
                address,
            };
            process.jumps.retain(|_, j| j.address != address);
            process.jumps.insert(cookie, jump);
            for i in 0..4 {
                self.memory.write(address + i, (cookie >> (16 * i)) as u16);
            }
            return;
        }
        if request.op == 14 {
            let address = linear(request.a);
            if address < BANK || address + 4 > address_space::KERNEL_START {
                fatal("invalid native jump buffer");
            }
            let cookie = (0..4).fold(0u64, |n, i| {
                n | (u64::from(self.memory.read(address + i)) << (16 * i))
            });
            let process = self.processes.active.as_mut().unwrap();
            let jump = process
                .jumps
                .get(&cookie)
                .cloned()
                .unwrap_or_else(|| fatal("expired native jump buffer"));
            if process
                .frames
                .get(jump.depth - 1)
                .is_none_or(|f| f.id != jump.frame.id)
            {
                fatal("native longjmp target has returned");
            }
            process.frames.truncate(jump.depth - 1);
            process.frames.push(jump.frame);
            let value = request.b as u16;
            process
                .frames
                .last_mut()
                .unwrap()
                .result(jump.result, u64::from(value.max(1)));
            process.jumps.retain(|_, j| {
                process
                    .frames
                    .get(j.depth - 1)
                    .is_some_and(|f| f.id == j.frame.id)
            });
            self.stack = jump.stack;
            return;
        }
        if self.processes.hosted {
            match request.op {
                15 => {
                    let token = self.processes.checkpoint;
                    if token != 0 {
                        self.processes
                            .active
                            .as_mut()
                            .unwrap()
                            .frames
                            .push(Frame::new(token, vec![], 0));
                    }
                }
                6 => {
                    let pointer = self.memory.kernel_allocate(request.a);
                    self.processes.frame().result(request.result, pointer);
                }
                7 => {
                    self.memory.kernel_release(request.a);
                    self.processes.frame().result(request.result, 0);
                }
                4 => {
                    self.processes.root_status = Some(request.a);
                    self.processes.active = None;
                    self.processes.runnable.clear();
                }
                8 => {
                    let slot = request.a;
                    if slot == 0
                        || slot >= self.processes.max_processes as u64
                        || self
                            .processes
                            .runnable
                            .iter()
                            .any(|p| u64::from(p.pid) == slot + 1)
                        || u64::from(self.processes.active.as_ref().unwrap().pid) == slot + 1
                    {
                        self.processes
                            .frame()
                            .result(request.result, u64::from(u16::MAX));
                    } else {
                        let mut child = Process {
                            pid: slot as u16 + 1,
                            parent: 0,
                            frames: self.processes.active.as_ref().unwrap().frames.clone(),
                            image: Some(self.clone_image()),
                            wait: None,
                            module: self.processes.active.as_ref().unwrap().module.clone(),
                            staged: None,
                            jumps: self.processes.active.as_ref().unwrap().jumps.clone(),
                            loading: None,
                        };
                        child.frames.last_mut().unwrap().result(request.result, 0);
                        self.processes.frame().result(request.result, 1);
                        self.processes.runnable.push_back(child);
                    }
                }
                9 | 10 => {
                    if u64::from(self.processes.active.as_ref().unwrap().pid) == request.a + 1 {
                        if request.op == 10 {
                            fatal("hosted context cannot drop itself and resume itself");
                        }
                        self.processes.frame().result(request.result, 0);
                        return;
                    }
                    let index = self
                        .processes
                        .runnable
                        .iter()
                        .position(|p| u64::from(p.pid) == request.a + 1)
                        .unwrap_or_else(|| fatal("hosted kernel selected an absent context"));
                    let mut target = self.processes.runnable.remove(index).unwrap();
                    self.processes.frame().result(request.result, 0);
                    let image = self.park_image();
                    let mut previous = self.processes.active.take().unwrap();
                    if request.op == 9 {
                        previous.image = Some(image);
                        self.processes.runnable.push_back(previous);
                    }
                    self.restore_image(target.image.take().unwrap());
                    self.processes.active = Some(target);
                }
                11 => {
                    let arity = self
                        .functions
                        .get(&(request.a as u32))
                        .unwrap_or_else(|| fatal("hosted exec entry is not registered"))
                        .1;
                    if arity != 1 {
                        fatal("hosted exec entry must accept one argument");
                    }
                    let owner = self.images.owner(request.a as u32);
                    self.processes.discard_staged(request.a as u32);
                    let template = owner
                        .as_ref()
                        .map(|m| m.template.as_slice())
                        .unwrap_or(&self.processes.global_template);
                    self.memory.write_from(BANK, template);
                    self.errno = owner.as_ref().map_or(0, |m| m.errno);
                    self.processes.active.as_mut().unwrap().module = owner;
                    self.memory.fill(2 * BANK..5 * BANK, 0);
                    self.stack = 2 * BANK + 1;
                    self.free = BTreeMap::from([(
                        5 * BANK + 1,
                        address_space::KERNEL_START - 5 * BANK - 1,
                    )]);
                    self.allocated.clear();
                    self.processes.active.as_mut().unwrap().frames =
                        vec![Frame::new(request.a as u32, vec![request.b], 0)];
                    self.processes.active.as_mut().unwrap().jumps.clear();
                    self.processes.active.as_mut().unwrap().loading = None;
                }
                12 => {
                    let token = self.images.syscall;
                    if token == 0 {
                        fatal("hosted syscall bridge is not registered");
                    }
                    self.processes
                        .active
                        .as_mut()
                        .unwrap()
                        .frames
                        .push(Frame::new(token, vec![request.a], request.result));
                }
                _ => fatal(
                    "cooperative process services cannot be mixed with hosted kernel contexts",
                ),
            }
            return;
        }
        match request.op {
            15 => self.rotate_process(),
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

impl Runtime {
    fn checkpoint_due(&mut self) -> u64 {
        let machine = &mut self.processes;
        if machine.hosted && machine.checkpoint == 0 {
            return 0;
        }
        machine.checkpoint_count = machine.checkpoint_count.wrapping_add(1);
        if machine.checkpoint_count < 256 {
            return 0;
        }
        machine.checkpoint_count = 0;
        if machine.hosted
            && machine
                .active
                .as_ref()
                .unwrap()
                .frames
                .iter()
                .any(|f| f.token == machine.checkpoint || f.token == self.images.syscall)
        {
            return 0;
        }
        let now = Instant::now();
        if machine
            .checkpoint_time
            .is_some_and(|last| now.duration_since(last) < Duration::from_millis(5))
        {
            return 0;
        }
        machine.checkpoint_time = Some(now);
        1
    }
}

#[no_mangle]
unsafe extern "C" fn rcc_native_process_checkpoint(ctx: *mut Runtime) -> u64 {
    unsafe { &mut *ctx }.checkpoint_due()
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_bind_checkpoint(
    ctx: *mut Runtime,
    entry: u64,
) -> u64 {
    let rt = unsafe { &mut *ctx };
    if !rt.processes.hosted
        || rt
            .functions
            .get(&(entry as u32))
            .is_none_or(|(_, arity)| *arity != 0)
    {
        fatal("invalid hosted checkpoint entry");
    }
    rt.processes.checkpoint = entry as u32;
    rt.processes.checkpoint_time = Some(Instant::now());

    0
}

#[no_mangle]
unsafe extern "C" fn rcc_native_ext_rcc_hosted_stopping(_ctx: *mut Runtime) -> u64 {
    u64::from(STOPPING.load(Ordering::Relaxed))
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
    if rt.processes.frame().id == 0 {
        rt.processes.next_frame = rt.processes.next_frame.checked_add(1)
            .unwrap_or_else(|| fatal("native frame identity space exhausted"));
        rt.processes.frame().id = rt.processes.next_frame;
    }
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
    rt.processes.active.as_mut().unwrap().jumps.retain(|_, j| j.frame.id != frame.id);
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
helper!(rcc_native_kernel_globals(rt, cells) {
    if rt.memory.kernel_allocate(cells) != pointer(address_space::KERNEL_START + 1) {
        fatal("cannot reserve native shared kernel globals");
    }
    0
});

/// # Safety
/// ctx is the live exclusively accessed runtime; its registered function tokens
/// must all name compiler-generated resumable steps with signature (ctx)->u64.
#[no_mangle]
unsafe extern "C" fn rcc_native_process_start(
    ctx: *mut Runtime,
    token: u64,
    argc: u64,
    argv: u64,
    hosted: u64,
) -> u64 {
    {
        let rt = unsafe { &mut *ctx };
        rt.enable_process_memory();
        if hosted != 0 {
            STOPPING.store(false, Ordering::Relaxed);
            // Signal handlers only set a flag. All guest work happens later, on its
            // execution thread, at an idle boundary, syscall or compiled checkpoint.
            unsafe {
                let mut action: libc::sigaction = std::mem::zeroed();
                action.sa_sigaction = stop_session as *const () as usize;
                libc::sigemptyset(&mut action.sa_mask);
                if libc::sigaction(libc::SIGTERM, &action, std::ptr::null_mut()) != 0 {
                    fatal("cannot install hosted shutdown handler");
                }
            }
        }
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
            hosted: hosted != 0,
            global_template: if hosted != 0 {
                rt.memory.cells(BANK..2 * BANK).collect()
            } else {
                Vec::new()
            },
            active: Some(Process {
                pid: 1,
                parent: 0,
                frames: vec![Frame::new(token as u32, args, 0)],
                image: None,
                wait: None,
                module: None,
                staged: None,
                jumps: BTreeMap::new(),
                loading: None,
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
        // Calls already return to this driver with a saved continuation. Check
        // their time slice here; only loops need extra compiler-generated code.
        let user_step = rt.processes.active.as_ref().is_some_and(|p| {
            !rt.processes.hosted
                || p.frames
                    .last()
                    .is_some_and(|f| rt.images.owner(f.token).is_some())
        });
        if user_step && rt.checkpoint_due() != 0 {
            // End the borrow before any future reentry into compiled code.
            rt.handle_request(Request {
                op: 15,
                a: 0,
                b: 0,
                result: 0,
            });
        }
        rt.images.collect(&mut rt.functions);
    }
}
