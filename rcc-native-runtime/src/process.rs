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

/// Compact new continuations use dense slots. Legacy packages with sparse SSA
/// identifiers retain a bounded dense prefix and a sparse overflow map.
#[derive(Clone, Default)]
struct Slots {
    dense: Vec<u64>,
    sparse: BTreeMap<u64, u64>,
}
impl Slots {
    fn insert(&mut self, slot: u64, value: u64) {
        if slot < 4096 {
            let slot = slot as usize;
            if self.dense.len() <= slot {
                self.dense.resize(slot + 1, 0);
            }
            self.dense[slot] = value;
        } else {
            self.sparse.insert(slot, value);
        }
    }
    fn get(&self, slot: u64) -> u64 {
        if slot < 4096 {
            self.dense.get(slot as usize).copied().unwrap_or(0)
        } else {
            self.sparse.get(&slot).copied().unwrap_or(0)
        }
    }
    fn clear(&mut self) {
        self.dense.clear();
        self.sparse.clear();
    }
}

#[derive(Clone)]
struct Frame {
    id: u64,
    token: u32,
    args: Vec<u64>,
    pc: u64,
    slots: Slots,
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
            slots: Slots::default(),
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
    loading: Option<images::Loading>,
    preparing: Option<images::Preparation>,
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
    just_returned: bool,
    checkpoint_time: Option<Instant>,
    pub(super) hosted: bool,
    global_template: Vec<u16>,
    extended_template: Option<(usize, Vec<u16>)>,
    active: Option<Process>,
    runnable: VecDeque<Process>,
    // Hosted scheduling is owned by the kernel. Parked contexts are indexed by
    // kernel slot (runtime pid minus one), never searched as a second run queue.
    parked: Vec<Option<Process>>,
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
    #[cfg(test)]
    pub(super) fn test_hosted() -> Self {
        Self {
            hosted: true,
            active: Some(Process {
                pid: 1,
                parent: 0,
                frames: Vec::new(),
                image: None,
                wait: None,
                module: None,
                staged: None,
                jumps: BTreeMap::new(),
                loading: None,
                preparing: None,
            }),
            ..Self::default()
        }
    }
    pub(super) fn preparing(&mut self) -> &mut Option<images::Preparation> {
        &mut self.active.as_mut().unwrap().preparing
    }
    pub(super) fn loading(&mut self) -> &mut Option<images::Loading> {
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
            preparing: None,
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
                    self.processes.parked.clear();
                }
                8 => {
                    let slot = request.a;
                    if slot == 0
                        || slot >= self.processes.max_processes as u64
                        || self
                            .processes
                            .parked
                            .get(slot as usize)
                            .is_some_and(Option::is_some)
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
                            preparing: None,
                        };
                        child.frames.last_mut().unwrap().result(request.result, 0);
                        self.processes.frame().result(request.result, 1);
                        if self.processes.parked.len() <= slot as usize {
                            self.processes
                                .parked
                                .resize_with(slot as usize + 1, || None);
                        }
                        self.processes.parked[slot as usize] = Some(child);
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
                    let mut target = usize::try_from(request.a)
                        .ok()
                        .and_then(|slot| self.processes.parked.get_mut(slot))
                        .and_then(Option::take)
                        .unwrap_or_else(|| fatal("hosted kernel selected an absent context"));
                    self.processes.frame().result(request.result, 0);
                    let image = self.park_image();
                    let mut previous = self.processes.active.take().unwrap();
                    if request.op == 9 {
                        previous.image = Some(image);
                        let slot = usize::from(previous.pid - 1);
                        if self.processes.parked.len() <= slot {
                            self.processes.parked.resize_with(slot + 1, || None);
                        }
                        self.processes.parked[slot] = Some(previous);
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
                    if let Some(previous) = self.processes.active.as_ref().unwrap().module.as_ref()
                    {
                        self.memory.fill(
                            previous.global_start..previous.global_start + previous.template.len(),
                            0,
                        );
                    }
                    self.memory
                        .write_from(BANK, &self.processes.global_template);
                    let mut global_range = None;
                    if let Some(image) = &owner {
                        self.memory.write_from(image.global_start, &image.template);
                        global_range =
                            Some(image.global_start..image.global_start + image.template.len());
                    } else if let Some((start, template)) = &self.processes.extended_template {
                        self.memory.write_from(*start, template);
                        global_range = Some(*start..*start + template.len());
                    }
                    self.errno = owner.as_ref().map_or(0, |m| m.errno);
                    self.processes.active.as_mut().unwrap().module = owner;
                    self.memory.fill(2 * BANK..5 * BANK, 0);
                    self.stack = 2 * BANK + 1;
                    self.free = BTreeMap::from([(
                        5 * BANK + 1,
                        address_space::KERNEL_START - 5 * BANK - 1,
                    )]);
                    self.allocated.clear();
                    if let Some(range) = global_range {
                        self.exclude_globals(range);
                    }
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
        // Aggregate results point into the callee's released frame until the
        // caller copies them. A hosted callback must not reuse that storage
        // between the return step and the caller's continuation.
        if machine.just_returned {
            return 0;
        }
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
helper!(rcc_native_process_slot(rt, slot) { rt.processes.frame().slots.get(slot) });
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
    rt.processes.just_returned = true;
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
            extended_template: if hosted != 0 {
                rt.extended_globals
                    .as_ref()
                    .map(|range| (range.start, rt.memory.cells(range.clone()).collect()))
            } else {
                None
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
                preparing: None,
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
            rt.processes.just_returned = false;
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

#[cfg(test)]
mod return_checkpoint_tests {
    use super::*;

    #[test]
    fn dense_and_legacy_sparse_slots_clear_clone_and_reuse() {
        let mut slots = Slots::default();
        let mut reference = BTreeMap::new();
        let mut seed = 17u64;
        for step in 0..10000 {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
            let slot = match step % 3 {
                0 => seed % 32,
                1 => 4090 + seed % 12,
                _ => seed,
            };
            slots.insert(slot, seed);
            reference.insert(slot, seed);
            assert_eq!(slots.get(slot), seed);
            if step % 127 == 0 {
                let child = slots.clone();
                slots.clear();
                assert_eq!(slots.get(slot), 0);
                for (&key, &value) in &reference {
                    assert_eq!(child.get(key), value);
                }
                reference.clear();
            }
        }
        slots.clear();
        slots.insert(0, 42);
        assert_eq!(slots.dense.len(), 1);
        assert_eq!(slots.get(4095), 0);
    }

    #[test]
    fn hosted_high_slot_switch_drop_and_reuse_are_indexed() {
        let mut rt = Runtime::new();
        rt.enable_process_memory();
        rt.processes = Machine::test_hosted();
        rt.processes.max_processes = 64;
        rt.processes
            .active
            .as_mut()
            .unwrap()
            .frames
            .push(Frame::new(1, vec![], 0));
        let request = |op, a| Request {
            op,
            a,
            b: 0,
            result: 1,
        };
        for round in 0..4 {
            rt.memory.write(BANK + 9, round);
            rt.handle_request(request(8, 63));
            assert!(rt.processes.runnable.is_empty());
            rt.handle_request(request(9, 63));
            assert_eq!(rt.processes.active.as_ref().unwrap().pid, 64);
            assert_eq!(rt.processes.frame().slots.get(0), 0);
            rt.memory.write(BANK + 9, 99);
            rt.handle_request(request(10, 0));
            assert_eq!(rt.processes.active.as_ref().unwrap().pid, 1);
            assert!(rt.processes.parked[63].is_none());
            assert_eq!(rt.memory.read(BANK + 9), round);
        }
    }

    #[test]
    fn hosted_checkpoint_waits_for_aggregate_return_copy() {
        let mut rt = Runtime::new();
        rt.enable_process_memory();
        let returned = 2 * BANK + 32;
        let mut callee = Frame::new(2, vec![], 1);
        callee.mark = returned;
        rt.stack = returned + 13;
        rt.memory.write_from(returned, &[11; 13]);
        rt.processes.hosted = true;
        rt.processes.checkpoint = 3;
        rt.processes.checkpoint_count = 255;
        rt.processes.checkpoint_time = Some(Instant::now() - Duration::from_millis(10));
        rt.processes.active = Some(Process {
            pid: 1,
            parent: 0,
            frames: vec![Frame::new(1, vec![], 0), callee],
            image: None,
            wait: None,
            module: None,
            staged: None,
            jumps: BTreeMap::new(),
            loading: None,
            preparing: None,
        });
        unsafe {
            rcc_native_process_return(&mut rt, pointer(returned));
        }
        // This is the scheduler boundary before the caller can copy the result.
        if rt.checkpoint_due() != 0 {
            rt.handle_request(Request {
                op: 15,
                a: 0,
                b: 0,
                result: 0,
            });
            let scratch = unsafe { rcc_native_process_frame(&mut rt, 13) };
            rt.memory.fill(linear(scratch)..linear(scratch) + 13, 99);
        }
        let copied: Vec<_> = rt.memory.cells(returned..returned + 13).collect();
        assert_eq!(copied, vec![11; 13]);
        assert_eq!(rt.processes.frame().slots.get(0), pointer(returned));
        // Once the caller continuation has consumed the return, callbacks run
        // normally and may reuse the released stack storage.
        rt.processes.just_returned = false;
        assert_eq!(rt.checkpoint_due(), 1);
        rt.handle_request(Request {
            op: 15,
            a: 0,
            b: 0,
            result: 0,
        });
        let scratch = unsafe { rcc_native_process_frame(&mut rt, 13) };
        assert_eq!(linear(scratch), returned);
        rt.memory.fill(returned..returned + 13, 99);
        assert_eq!(copied, vec![11; 13]);
    }
}
