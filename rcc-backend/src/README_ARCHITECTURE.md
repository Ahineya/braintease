# V2 Backend Architecture

> Module map for `rcc-backend`. Machine ABI: [`docs/arch/`](../../docs/arch/README.md). Some examples below still use old crate names (`rcc_ir`).

## Design Principles

The V2 backend follows strict encapsulation to ensure safety and prevent misuse:

1. **Make illegal states unrepresentable**
2. **Hide implementation complexity**
3. **No escape hatches to internals**
4. **If users need something, add a safe method**

## Module Structure

```
v2/
├── function/              # Function generation (encapsulated)
│   ├── mod.rs            # Public API exports
│   ├── builder.rs        # FunctionBuilder (PUBLIC)
│   ├── lowering.rs       # FunctionLowering (internal)
│   ├── calling_convention.rs  # CallingConvention (internal)
│   └── tests/            # Internal tests with access to internals
│
├── regmgmt/              # Register management (encapsulated)
│   ├── mod.rs           # Public API exports
│   ├── pressure.rs      # RegisterPressureManager (PUBLIC)
│   ├── allocator.rs     # RegAllocV2 (internal)
│   ├── bank.rs          # BankInfo types
│   └── tests.rs         # Internal tests
│
├── instr/                # Instruction lowering
│   ├── mod.rs           # Public exports
│   ├── load.rs          # Load lowering (PUBLIC)
│   ├── store.rs         # Store lowering (PUBLIC)
│   └── tests/           # Instruction tests
│
├── naming/               # Centralized naming
│   └── mod.rs           # NameGenerator (PUBLIC)
│
└── tests/                # Integration tests
    └── integration_tests.rs  # Tests using ONLY public APIs
```

## Public API

Users of the V2 backend should ONLY use:

- `FunctionBuilder` - Safe function generation
- `CallArg` - Argument types for calls
- `RegisterPressureManager` - Register allocation (if needed directly)
- `lower_load/lower_store` - Instruction lowering
- `NameGenerator` - Unique naming

## Example Usage

```rust
use rcc_ir::{FunctionBuilder, CallArg};

let mut builder = FunctionBuilder::new();

builder.begin_function(10);  // 10 local slots

let param = builder.load_parameter(0);

// Make a call - ALL complexity handled automatically
let (result, _) = builder.call_function(
    0x200,  // address
    2,      // bank
    vec![CallArg::Scalar(param)],
    false   // returns scalar
);

builder.end_function(Some((result, None)));

let instructions = builder.build();
```

## Safety Guarantees

The FunctionBuilder API prevents:
- Emitting epilogue before prologue
- Forgetting stack cleanup after calls
- Accessing locals before prologue
- Mismatched call/cleanup sequences
- Breaking internal invariants

## Testing Strategy

- **Internal tests** (function/tests/, regmgmt/tests/): Test implementation details
- **Integration tests** (tests/integration_tests.rs): Test through public API only
- No test should import internal modules unless it's in the module's own test directory