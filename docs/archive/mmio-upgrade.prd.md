> **Deprecated (historical).** This file is not a current specification.
> Current architecture lives in [`docs/arch/`](../arch/README.md).
> See [`docs/archive/README.md`](README.md) for why this document was archived.

---

PRD — Ripple VM Minimal Packed IO Block (with 32-word header)

Objective

Expose a contiguous, word-addressed MMIO header at bank 0 / words 0..31 for ultra-cheap Brainfuck access, with TEXT40 VRAM starting at word 32. Fixed addresses, no discovery needed.

Addressing & Types
•	All addresses are u16 words.
•	“low8” means only the low byte is used/read.
•	All registers live in bank 0.

⸻

Memory Map

Header (reserved 32 words) — bank0:[0..31]

Word	Name	R/W	Type	Semantics
0	TTY_OUT	W	u16	low8 → host stdout (immediate). Host may transiently mark busy.
1	TTY_STATUS	R	u16	bit0: ready (1=ready,0=busy).
2	TTY_IN_POP	R	u16	Pops next input byte; returns in low8. 0 if empty.
3	TTY_IN_STATUS	R	u16	bit0: has_byte.
4	RNG	R	u16	Reading advances PRNG; returns next u16.
5	DISP_MODE	RW	u16	0=OFF, 1=TTY passthrough, 2=TEXT40.
6	DISP_STATUS	R	u16	bit0: ready, bit1: flush_done.
7	DISP_CTL	RW	u16	bit0: ENABLE, bit1: CLEAR (edge; auto-clear).
8	DISP_FLUSH	W	u16	Write 1 to present current TEXT40_VRAM; host sets flush_done.
9–31	RESERVED	—	—	Future extensions (keep zero now; reads return 0, writes ignored).

TEXT40 VRAM — bank0:[32..1031]
•	1000 words (40×25 cells) starting at word 32.
•	Cell format: word = (attr << 8) | ascii (use attr=0 until you implement colors).

Span summary
•	Header: words 0..31 (32 words).
•	VRAM: words 32..1031 (1000 words).
•	General RAM available at word 1032+.

⸻

Bitfields

// TTY_STATUS
pub const TTY_READY: u16 = 1 << 0;      // bit0

// TTY_IN_STATUS
pub const TTY_HAS_BYTE: u16 = 1 << 0;   // bit0

// DISP_MODE
pub const DISP_OFF:    u16 = 0;
pub const DISP_TTY:    u16 = 1;
pub const DISP_TEXT40: u16 = 2;

// DISP_STATUS
pub const DISP_READY:      u16 = 1 << 0;  // bit0
pub const DISP_FLUSH_DONE: u16 = 1 << 1;  // bit1

// DISP_CTL
pub const DISP_ENABLE: u16 = 1 << 0;    // bit0
pub const DISP_CLEAR:  u16 = 1 << 1;    // bit1 (edge-triggered)


⸻

Program Examples (Ripple-ish)

Print to TTY

LI  A0, 'A'
STORE A0, 0, 0          ; [0] TTY_OUT

Read a key if available

LOAD T0, 0, 3           ; [3] TTY_IN_STATUS
ANDI T0, T0, 1
BEQ  T0, ZR, no_key
LOAD A1, 0, 2           ; [2] TTY_IN_POP (pops one byte)

Init TEXT40 and write “Hi” at top-left

LI  A0, 2               ; TEXT40
STORE A0, 0, 5          ; [5] DISP_MODE
LI  A1, 1               ; ENABLE
STORE A1, 0, 7          ; [7] DISP_CTL

LI  T0, 0x1F48          ; 'H'
STORE T0, 0, 32         ; VRAM[0] at word 32
LI  T1, 0x1F69          ; 'i'
STORE T1, 0, 33         ; VRAM[1] at word 33

LI  T2, 1
STORE T2, 0, 8          ; [8] DISP_FLUSH

Random number

LOAD A0, 0, 4           ; [4] RNG


⸻

VM Implementation Notes
1.	Load hook
Intercept reads for words 0..8: return dynamic values.
Words 9..31: return 0 for now.
Words 32..1031: read from memory[].
2.	Store hook
Intercept writes for TTY_OUT, DISP_MODE, DISP_CTL, DISP_FLUSH.
Words 2/TTY_IN_POP are read-only (ignore stores).
Words 9..31: ignore for now.
Words 32..1031: write-through to memory[].
3.	TTY timing
On TTY_OUT store: emit low8, set TTY_STATUS=0 for one VM step (or immediately restore to 1 if you want zero-latency), then TTY_STATUS=1.
4.	Display
•	DISP_CTL.CLEAR: zero memory[32..1031], then auto-clear that bit, keep ENABLE latched.
•	DISP_FLUSH=1: clear DISP_STATUS.flush_done, render 40×25 cells from memory[32..1031], then set flush_done and ensure ready=1.
•	If DISP_MODE!=TEXT40 or !ENABLE, FLUSH is a no-op (but still sets flush_done=1).
5.	RNG
Each RNG read advances state and returns a u16. For now always seeded by host with a fixed PRNG (e.g., LCG).
6.	Banking
Devices and VRAM are defined only in bank 0. Ignore bank register for these addresses.

⸻

Rust Constants (drop-in)

pub const HDR_TTY_OUT: usize       = 0;
pub const HDR_TTY_STATUS: usize    = 1;
pub const HDR_TTY_IN_POP: usize    = 2;
pub const HDR_TTY_IN_STATUS: usize = 3;
pub const HDR_RNG: usize           = 4;
pub const HDR_DISP_MODE: usize     = 5;
pub const HDR_DISP_STATUS: usize   = 6;
pub const HDR_DISP_CTL: usize      = 7;
pub const HDR_DISP_FLUSH: usize    = 8;

// Reserved: 9..31

pub const TEXT40_BASE_WORD: usize  = 32;         // start of VRAM
pub const TEXT40_WORDS: usize      = 40 * 25;    // 1000
pub const TEXT40_LAST_WORD: usize  = TEXT40_BASE_WORD + TEXT40_WORDS - 1; // 1031


⸻

Rationale
•	BF-fast: fixed header indices with no pointer math; VRAM at a constant base (32).
•	No discovery needed: dimensions and offsets are constant by contract.
•	Forward-compatible: 23 spare header words for future devices without reshuffling VRAM or breaking binaries.
