use clap::{Parser, ArgAction};
use std::fs;
use std::io::{self, Read, Write};
use std::time::Instant;
use bf_wasm::engine::{Machine, Program, Stop};

#[derive(Parser, Debug)]
#[command(name = "bf")]
#[command(about = "Optimizing Brainfuck interpreter", long_about = None)]
struct Args {
    /// Input Brainfuck file (or read from stdin if not provided)
    file: Option<String>,

    /// Execute inline Brainfuck code
    #[arg(short = 'e', long, value_name = "CODE")]
    execute: Option<String>,

    /// Cell size in bits (8, 16, or 32)
    #[arg(long, value_name = "BITS", default_value = "16")]
    cell_size: u8,

    /// Disable cell wrapping behavior
    #[arg(long = "no-wrap", action = ArgAction::SetFalse, default_value_t = true)]
    wrap: bool,

    /// Disable tape pointer wrapping behavior  
    #[arg(long = "no-wrap-tape", action = ArgAction::SetFalse, default_value_t = true)]
    wrap_tape: bool,

    /// Tape size (number of cells)
    #[arg(long, value_name = "SIZE", default_value = "30000")]
    tape_size: usize,

    /// Show execution statistics
    #[arg(long)]
    stat: bool,

    /// Show verbose output during optimization and execution
    #[arg(long)]
    verbose: bool,

    /// Output optimized code instead of executing
    #[arg(long)]
    emit_optimized: bool,

    /// Skip optimization phase
    #[arg(long)]
    no_optimize: bool,

    /// Compile hot regions to native code (wrapping 16-bit mode)
    #[arg(long, conflicts_with = "emit_optimized")]
    jit: bool,
}

#[derive(Debug, Clone)]
enum Op {
    Add(i32),
    Move(i32),
    Loop(Vec<Op>),
    Input,
    Output,
    Clear,
    Set(i32),         // Set cell to constant value
    MulAdd(i32, i32), // [>+++<-] becomes MulAdd(1, 3)
    ScanLeft(i32),
    ScanRight(i32),
}

trait Cell: Copy + Default + PartialEq {
    fn from_u8(v: u8) -> Self;
    fn to_u8(self) -> u8;
    fn wrapping_add(self, n: i32) -> Self;
    fn saturating_add(self, n: i32) -> Self;
}

impl Cell for u8 {
    fn from_u8(v: u8) -> Self {
        v
    }
    
    fn to_u8(self) -> u8 {
        self
    }
    
    fn wrapping_add(self, n: i32) -> Self {
        if n >= 0 {
            self.wrapping_add(n as u8)
        } else {
            self.wrapping_sub((-n) as u8)
        }
    }
    
    fn saturating_add(self, n: i32) -> Self {
        if n >= 0 {
            self.saturating_add(n as u8)
        } else {
            self.saturating_sub((-n) as u8)
        }
    }
}

impl Cell for u16 {
    fn from_u8(v: u8) -> Self {
        v as u16
    }
    
    fn to_u8(self) -> u8 {
        (self & 0xFF) as u8
    }
    
    fn wrapping_add(self, n: i32) -> Self {
        if n >= 0 {
            self.wrapping_add(n as u16)
        } else {
            self.wrapping_sub((-n) as u16)
        }
    }
    
    fn saturating_add(self, n: i32) -> Self {
        if n >= 0 {
            self.saturating_add(n as u16)
        } else {
            self.saturating_sub((-n) as u16)
        }
    }
}

impl Cell for u32 {
    fn from_u8(v: u8) -> Self {
        v as u32
    }
    
    fn to_u8(self) -> u8 {
        (self & 0xFF) as u8
    }
    
    fn wrapping_add(self, n: i32) -> Self {
        if n >= 0 {
            self.wrapping_add(n as u32)
        } else {
            self.wrapping_sub((-n) as u32)
        }
    }
    
    fn saturating_add(self, n: i32) -> Self {
        if n >= 0 {
            self.saturating_add(n as u32)
        } else {
            self.saturating_sub((-n) as u32)
        }
    }
}

struct Interpreter<T: Cell> {
    tape: Vec<T>,
    ptr: usize,
    wrap: bool,
    wrap_tape: bool,
}

impl<T: Cell> Interpreter<T> {
    fn new(tape_size: usize, wrap: bool, wrap_tape: bool) -> Self {
        Self {
            tape: vec![T::default(); tape_size],
            ptr: 0,
            wrap,
            wrap_tape,
        }
    }

    fn run(&mut self, ops: &[Op]) {
        for op in ops {
            self.execute(op);
        }
    }

    fn execute(&mut self, op: &Op) {
        match op {
            Op::Add(n) => {
                if self.wrap {
                    self.tape[self.ptr] = self.tape[self.ptr].wrapping_add(*n);
                } else {
                    self.tape[self.ptr] = self.tape[self.ptr].saturating_add(*n);
                }
            }
            Op::Move(n) => {
                let new_ptr = (self.ptr as i64) + (*n as i64);
                if self.wrap_tape {
                    // Wrap around the tape
                    if new_ptr < 0 {
                        self.ptr = ((new_ptr % self.tape.len() as i64 + self.tape.len() as i64) % self.tape.len() as i64) as usize;
                    } else {
                        self.ptr = (new_ptr as usize) % self.tape.len();
                    }
                } else {
                    // Clamp to tape bounds (no wrapping)
                    if new_ptr < 0 {
                        self.ptr = 0;
                    } else if new_ptr >= self.tape.len() as i64 {
                        self.ptr = self.tape.len() - 1;
                    } else {
                        self.ptr = new_ptr as usize;
                    }
                }
            }
            Op::Loop(ops) => {
                while self.tape[self.ptr] != T::default() {
                    self.run(ops);
                }
            }
            Op::Input => {
                let mut buf = [0];
                io::stdin().read_exact(&mut buf).unwrap_or(());
                self.tape[self.ptr] = T::from_u8(buf[0]);
            }
            Op::Output => {
                io::stdout().write_all(&[self.tape[self.ptr].to_u8()]).unwrap();
                io::stdout().flush().unwrap();
            }
            Op::Clear => {
                self.tape[self.ptr] = T::default();
            }
            Op::Set(value) => {
                // Set cell to a specific value
                self.tape[self.ptr] = match std::mem::size_of::<T>() {
                    1 => T::from_u8((*value & 0xFF) as u8),
                    2 => unsafe { std::mem::transmute_copy(&((*value & 0xFFFF) as u16)) },
                    4 => unsafe { std::mem::transmute_copy(&(*value as u32)) },
                    _ => unreachable!()
                };
            }
            Op::MulAdd(offset, factor) => {
                let val = self.tape[self.ptr];
                if val != T::default() {
                    let target = (self.ptr as i64) + (*offset as i64);
                    let target_ptr = if self.wrap_tape {
                        if target < 0 {
                            ((target % self.tape.len() as i64 + self.tape.len() as i64) % self.tape.len() as i64) as usize
                        } else {
                            (target as usize) % self.tape.len()
                        }
                    } else {
                        if target < 0 {
                            0
                        } else if target >= self.tape.len() as i64 {
                            self.tape.len() - 1
                        } else {
                            target as usize
                        }
                    };
                    
                    // MulAdd needs to work with the actual cell value, not just u8
                    let result = match std::mem::size_of::<T>() {
                        1 => (val.to_u8() as i32 * factor),
                        2 => {
                            let v = unsafe { std::mem::transmute_copy::<T, u16>(&val) };
                            (v as i32 * factor)
                        },
                        4 => {
                            let v = unsafe { std::mem::transmute_copy::<T, u32>(&val) };
                            // Need to handle potential overflow for 32-bit multiplication
                            ((v as i64 * *factor as i64) & 0xFFFFFFFF) as i32
                        },
                        _ => unreachable!()
                    };
                    
                    if self.wrap {
                        self.tape[target_ptr] = self.tape[target_ptr].wrapping_add(result);
                    } else {
                        self.tape[target_ptr] = self.tape[target_ptr].saturating_add(result);
                    }
                    self.tape[self.ptr] = T::default();
                }
            }
            Op::ScanLeft(step) => {
                while self.tape[self.ptr] != T::default() {
                    let new_ptr = (self.ptr as i64) - (step.abs() as i64);
                    if self.wrap_tape {
                        if new_ptr < 0 {
                            self.ptr = ((new_ptr % self.tape.len() as i64 + self.tape.len() as i64) % self.tape.len() as i64) as usize;
                        } else {
                            self.ptr = new_ptr as usize;
                        }
                    } else {
                        if new_ptr < 0 {
                            self.ptr = 0;
                            break; // Stop at boundary
                        } else {
                            self.ptr = new_ptr as usize;
                        }
                    }
                    if self.ptr == 0 && !self.wrap_tape {
                        break; // Stop at the beginning if not wrapping
                    }
                }
            }
            Op::ScanRight(step) => {
                while self.tape[self.ptr] != T::default() {
                    let new_ptr = (self.ptr as i64) + (*step as i64);
                    if self.wrap_tape {
                        self.ptr = (new_ptr as usize) % self.tape.len();
                    } else {
                        if new_ptr >= self.tape.len() as i64 {
                            self.ptr = self.tape.len() - 1;
                            break; // Stop at boundary
                        } else {
                            self.ptr = new_ptr as usize;
                        }
                    }
                }
            }
        }
    }
}

fn preprocess_collapse(code: &str) -> String {
    let mut result = String::new();
    let mut chars = code.chars().peekable();
    
    while let Some(ch) = chars.next() {
        match ch {
            '+' | '-' => {
                // Count consecutive + and - operations
                let mut count = if ch == '+' { 1i32 } else { -1i32 };
                while let Some(&next_ch) = chars.peek() {
                    match next_ch {
                        '+' => {
                            chars.next();
                            count += 1;
                        }
                        '-' => {
                            chars.next();
                            count -= 1;
                        }
                        _ => break,
                    }
                }
                // Emit the net result
                if count > 0 {
                    for _ in 0..count {
                        result.push('+');
                    }
                } else if count < 0 {
                    for _ in 0..(-count) {
                        result.push('-');
                    }
                }
            }
            '>' | '<' => {
                // Count consecutive > and < operations
                let mut count = if ch == '>' { 1i32 } else { -1i32 };
                while let Some(&next_ch) = chars.peek() {
                    match next_ch {
                        '>' => {
                            chars.next();
                            count += 1;
                        }
                        '<' => {
                            chars.next();
                            count -= 1;
                        }
                        _ => break,
                    }
                }
                // Emit the net result
                if count > 0 {
                    for _ in 0..count {
                        result.push('>');
                    }
                } else if count < 0 {
                    for _ in 0..(-count) {
                        result.push('<');
                    }
                }
            }
            // Pass through other characters unchanged
            c if "[].,".contains(c) => result.push(c),
            _ => {} // Skip non-BF characters
        }
    }
    
    result
}

fn parse(code: &str) -> Vec<Op> {
    let mut ops = Vec::new();
    let mut chars = code.chars().peekable();
    
    while let Some(ch) = chars.next() {
        match ch {
            '+' => {
                let mut count = 1;
                while chars.peek() == Some(&'+') {
                    chars.next();
                    count += 1;
                }
                ops.push(Op::Add(count));
            }
            '-' => {
                let mut count = -1;
                while chars.peek() == Some(&'-') {
                    chars.next();
                    count -= 1;
                }
                ops.push(Op::Add(count));
            }
            '>' => {
                let mut count = 1;
                while chars.peek() == Some(&'>') {
                    chars.next();
                    count += 1;
                }
                ops.push(Op::Move(count));
            }
            '<' => {
                let mut count = -1;
                while chars.peek() == Some(&'<') {
                    chars.next();
                    count -= 1;
                }
                ops.push(Op::Move(count));
            }
            '[' => {
                let mut depth = 1;
                let mut loop_code = String::new();
                
                while depth > 0 {
                    match chars.next() {
                        Some('[') => {
                            loop_code.push('[');
                            depth += 1;
                        }
                        Some(']') => {
                            depth -= 1;
                            if depth > 0 {
                                loop_code.push(']');
                            }
                        }
                        Some(c) => loop_code.push(c),
                        None => break,
                    }
                }
                
                let loop_ops = parse(&loop_code);
                ops.push(Op::Loop(loop_ops));
            }
            ',' => ops.push(Op::Input),
            '.' => ops.push(Op::Output),
            _ => {}
        }
    }
    
    ops
}

fn optimize(ops: Vec<Op>) -> Vec<Op> {
    // Pattern-based optimizations and Clear+Add->Set
    // (consecutive ops are already collapsed during parsing)
    optimize_patterns(ops)
}

fn optimize_patterns(ops: Vec<Op>) -> Vec<Op> {
    let mut result = Vec::new();
    let mut i = 0;
    
    while i < ops.len() {
        match &ops[i] {
            Op::Clear => {
                // Look ahead for Add operation
                if i + 1 < ops.len() {
                    if let Op::Add(n) = &ops[i + 1] {
                        // Replace Clear + Add with Set
                        result.push(Op::Set(*n));
                        i += 2;
                        continue;
                    }
                }
                result.push(ops[i].clone());
                i += 1;
            }
            Op::Loop(loop_ops) => {
                // First optimize the contents
                let optimized_loop = optimize_patterns(loop_ops.clone());
                
                // Pattern: [-] or [+] -> Clear
                if optimized_loop.len() == 1 {
                    if let Op::Add(n) = optimized_loop[0] {
                        if n == -1 || n == 1 {
                            result.push(Op::Clear);
                            i += 1;
                            continue;
                        }
                    }
                }
                
                // Pattern: [>] or [>>...] -> ScanRight/ScanLeft
                if optimized_loop.len() == 1 {
                    if let Op::Move(n) = optimized_loop[0] {
                        if n > 0 {
                            result.push(Op::ScanRight(n));
                            i += 1;
                            continue;
                        } else if n < 0 {
                            result.push(Op::ScanLeft(n));
                            i += 1;
                            continue;
                        }
                    }
                }
                
                // Pattern: [->+<] or [->+++<] etc. -> MulAdd
                if optimized_loop.len() == 4 {
                    if let (Op::Add(-1), Op::Move(m1), Op::Add(factor), Op::Move(m2)) = 
                        (&optimized_loop[0], &optimized_loop[1], &optimized_loop[2], &optimized_loop[3]) {
                        if m1 + m2 == 0 && *m1 != 0 {
                            result.push(Op::MulAdd(*m1, *factor));
                            i += 1;
                            continue;
                        }
                    }
                }
                
                result.push(Op::Loop(optimized_loop));
                i += 1;
            }
            _ => {
                result.push(ops[i].clone());
                i += 1;
            }
        }
    }
    
    result
}

fn ops_to_brainfuck(ops: &[Op]) -> String {
    let mut result = String::new();
    
    for op in ops {
        match op {
            Op::Add(n) => {
                if *n > 0 {
                    for _ in 0..*n {
                        result.push('+');
                    }
                } else if *n < 0 {
                    for _ in 0..(-n) {
                        result.push('-');
                    }
                }
            }
            Op::Move(n) => {
                if *n > 0 {
                    for _ in 0..*n {
                        result.push('>');
                    }
                } else if *n < 0 {
                    for _ in 0..(-n) {
                        result.push('<');
                    }
                }
            }
            Op::Loop(loop_ops) => {
                result.push('[');
                result.push_str(&ops_to_brainfuck(loop_ops));
                result.push(']');
            }
            Op::Input => result.push(','),
            Op::Output => result.push('.'),
            Op::Clear => result.push_str("[-]"),
            Op::Set(n) => {
                result.push_str("[-]");
                if *n > 0 {
                    for _ in 0..*n {
                        result.push('+');
                    }
                } else if *n < 0 {
                    for _ in 0..(-n) {
                        result.push('-');
                    }
                }
            }
            Op::MulAdd(offset, factor) => {
                // Emit as a loop pattern [->+++<] or similar
                result.push('[');
                result.push('-');
                if *offset > 0 {
                    for _ in 0..*offset {
                        result.push('>');
                    }
                } else {
                    for _ in 0..(-offset) {
                        result.push('<');
                    }
                }
                if *factor > 0 {
                    for _ in 0..*factor {
                        result.push('+');
                    }
                } else {
                    for _ in 0..(-factor) {
                        result.push('-');
                    }
                }
                if *offset > 0 {
                    for _ in 0..*offset {
                        result.push('<');
                    }
                } else {
                    for _ in 0..(-offset) {
                        result.push('>');
                    }
                }
                result.push(']');
            }
            Op::ScanLeft(step) => {
                result.push('[');
                for _ in 0..step.abs() {
                    result.push('<');
                }
                result.push(']');
            }
            Op::ScanRight(step) => {
                result.push('[');
                for _ in 0..*step {
                    result.push('>');
                }
                result.push(']');
            }
        }
    }
    
    result
}

fn run_interpreter<T: Cell>(tape_size: usize, wrap: bool, wrap_tape: bool, optimized: &[Op]) {
    let mut interpreter = Interpreter::<T>::new(tape_size, wrap, wrap_tape);
    interpreter.run(optimized);
}

// Compatibility modes retain literal operations. Arithmetic cancellation and
// affine/scan reductions are not generally valid for saturating cells or
// clamped pointers, and the old optimizer did not account for those settings.
fn parse_literal(code: &str) -> Result<Vec<Op>, String> {
    let mut scopes = vec![Vec::new()];
    for (offset, byte) in code.bytes().enumerate() {
        let op = match byte {
            b'+' => Op::Add(1), b'-' => Op::Add(-1),
            b'>' => Op::Move(1), b'<' => Op::Move(-1),
            b',' => Op::Input, b'.' => Op::Output,
            b'[' => { scopes.push(Vec::new()); continue; }
            b']' => {
                if scopes.len() == 1 { return Err(format!("unmatched closing bracket at source byte {offset}")); }
                Op::Loop(scopes.pop().unwrap())
            }
            _ => continue,
        };
        scopes.last_mut().unwrap().push(op);
    }
    if scopes.len() != 1 { return Err("unmatched opening bracket".into()); }
    Ok(scopes.pop().unwrap())
}

fn run_fast(
    program: &Program,
    tape_size: usize,
    native: bool,
    statistics: bool,
) -> Result<u64, Box<dyn std::error::Error>> {
    let mut machine = Machine::with_wrapping_tape(tape_size)?;
    let mut jit = native
        .then(|| bf_wasm::engine::jit::Jit::new(program))
        .transpose()?;
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    loop {
        let stop = if let Some(jit) = &mut jit {
            jit.run(&mut machine, 65_536)?
        } else {
            machine.run(program, 65_536)?
        };
        match stop {
            Stop::Budget => {}
            Stop::Halted => {
                output.flush()?;
                if statistics {
                    if let Some(jit) = &jit {
                        eprintln!("JIT: {:?}", jit.stats());
                    }
                }
                return Ok(machine.dispatches());
            }
            Stop::Output(value) => output.write_all(&[value])?,
            Stop::Input => {
                output.flush()?;
                let mut value = [0u8];
                match input.read_exact(&mut value) {
                    Ok(()) => {}
                    Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => {}
                    Err(error) => return Err(error.into()),
                }
                machine.provide_input(value[0])?;
            }
            Stop::Hook(_) => unreachable!("plain bf does not enable host markers"),
        }
    }
}

fn main() {
    let args = Args::parse();
    
    // Validate cell size
    match args.cell_size {
        8 | 16 | 32 => {}
        _ => {
            eprintln!("Error: cell-size must be 8, 16, or 32");
            std::process::exit(1);
        }
    }
    if args.tape_size == 0 {
        eprintln!("Error: tape-size must be positive");
        std::process::exit(1);
    }
    if args.jit && !(args.cell_size == 16 && args.wrap && args.wrap_tape) {
        eprintln!("Error: --jit requires wrapping 16-bit cells and a wrapping tape");
        std::process::exit(1);
    }
    
    let code = if let Some(inline_code) = args.execute {
        // Use -e flag code
        inline_code
    } else if let Some(file) = args.file {
        // Read from file
        match fs::read_to_string(&file) {
            Ok(content) => content,
            Err(e) => {
                eprintln!("Error reading file '{}': {}", file, e);
                std::process::exit(1);
            }
        }
    } else {
        // Read from stdin
        let mut buffer = String::new();
        match io::stdin().read_to_string(&mut buffer) {
            Ok(_) => buffer,
            Err(e) => {
                eprintln!("Error reading from stdin: {}", e);
                std::process::exit(1);
            }
        }
    };
    
    let start = if args.stat || args.verbose {
        Some(Instant::now())
    } else {
        None
    };

    if !args.emit_optimized && args.cell_size == 16 && args.wrap && args.wrap_tape {
        let result = Program::parse(&code, !args.no_optimize, false)
            .map_err(|error| error.to_string())
            .and_then(|program| {
                if args.verbose {
                    eprintln!("Parsed {} IR instructions, {} optimized regions", program.instruction_count(), program.optimization_count());
                }
                run_fast(&program, args.tape_size, args.jit, args.stat || args.verbose).map_err(|error| error.to_string())
            });
        match result {
            Ok(dispatches) => {
                if let Some(start) = start {
                    eprintln!("Execution time: {:.6} seconds; {} IR dispatches", start.elapsed().as_secs_f64(), dispatches);
                }
            }
            Err(error) => { eprintln!("Error: {error}"); std::process::exit(1); }
        }
        return;
    }

    // Validate brackets even for the older emission/compatibility paths.
    let literal = parse_literal(&code).unwrap_or_else(|error| {
        eprintln!("Error: {error}"); std::process::exit(1);
    });
    if !args.emit_optimized || !args.wrap || !args.wrap_tape {
        if args.emit_optimized {
            println!("{}", ops_to_brainfuck(&literal));
        } else {
            if args.verbose { eprintln!("Using literal compatibility execution for these cell/pointer settings"); }
            match args.cell_size {
                8 => run_interpreter::<u8>(args.tape_size, args.wrap, args.wrap_tape, &literal),
                16 => run_interpreter::<u16>(args.tape_size, args.wrap, args.wrap_tape, &literal),
                32 => run_interpreter::<u32>(args.tape_size, args.wrap, args.wrap_tape, &literal),
                _ => unreachable!(),
            }
        }
        if let Some(start) = start { eprintln!("Execution time: {:.6} seconds", start.elapsed().as_secs_f64()); }
        return;
    }
    drop(literal);
    
    if args.verbose {
        eprintln!("Preprocessing {} bytes of code...", code.len());
    }
    
    let preprocess_start = if args.verbose { Some(Instant::now()) } else { None };
    let collapsed_code = preprocess_collapse(&code);
    if let Some(ps) = preprocess_start {
        eprintln!("Preprocessed to {} bytes in {:.3}s", collapsed_code.len(), ps.elapsed().as_secs_f64());
    }
    
    // Don't emit here - we'll emit after full optimization
    
    if args.verbose {
        eprintln!("Parsing preprocessed code...");
    }
    
    let parse_start = if args.verbose { Some(Instant::now()) } else { None };
    let ops = parse(&collapsed_code);
    if let Some(ps) = parse_start {
        eprintln!("Parsed {} operations in {:.3}s", ops.len(), ps.elapsed().as_secs_f64());
    }
    
    let optimized = if args.no_optimize {
        if args.verbose {
            eprintln!("Skipping optimization phase (--no-optimize flag)");
        }
        ops
    } else {
        if args.verbose {
            eprintln!("Starting optimization...");
        }
        
        let opt_start = if args.verbose { Some(Instant::now()) } else { None };
        let result = optimize(ops);
        if let Some(os) = opt_start {
            eprintln!("Optimization complete: {} operations in {:.3}s", 
                     result.len(), os.elapsed().as_secs_f64());
        }
        result
    };
    
    // If requested, emit the fully optimized code
    if args.emit_optimized {
        let optimized_code = ops_to_brainfuck(&optimized);
        if args.verbose {
            eprintln!("Emitting {} bytes of optimized code", optimized_code.len());
        }
        println!("{}", optimized_code);
        return;
    }
    
}
