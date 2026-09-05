# Luatrix

Luatrix is a dependency-free C++17 Lua obfuscator derived from the public Hercules pipeline. It runs the complete compatible transformation pipeline automatically and intentionally exposes one command shape:

```bash
luatrix <input> <out>
```

There are no feature modes, presets, target flags, seeds, or output flags. Luatrix detects Lua, Luau, and GLua from the input and applies every compatible pass. Lua 5.1–5.4 and Luau-compatible loaders are selected automatically, with fresh randomness on each run.

## Build

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic src/luatrix.cpp -o luatrix
```

## Run

```bash
./luatrix script.lua script_obfuscated.lua
```

The output begins with the Luatrix watermark: `LTRIX`.

## Features

Luatrix currently includes 15 pipeline features:

- Dynamic Code
- Opaque Predicates
- String Encoding
- String To Expressions
- Function Inlining
- Variable Renaming
- Virtual Machine
- Anti Tamper
- Anti Debug
- Control Flow
- Garbage Code
- Compressor
- Function Wrapping
- Bytecode Encoding
- Watermark (LTRIX)

The lexer protects strings, comments, long-bracket strings, identifiers, and punctuation before transformations are applied. The anti-debug pass checks for an already-installed debug hook but does not require the debug library to exist. Lua-only VM and bytecode passes are skipped automatically for Luau and GLua inputs.

## Randomized virtual opcodes

The VM does not use a fixed opcode vocabulary. For every output it generates a fresh virtual instruction vocabulary. Each token is translated through a new numeric ID table before reaching its handler, for example:

```
ABC -> 18492031 -> return handler
XZX -> 771204006 -> no-op handler
```

A later output receives different token names, numeric IDs, local variable names, and several shuffled no-op instructions. The dispatcher has no stable `LOAD`, `RET`, or `NOP` labels, making static signatures based on a permanent opcode vocabulary ineffective. Explicit deterministic seeds are not exposed by the CLI, so normal runs always receive fresh mappings.

## License and attribution

This native port retains the Apache-2.0 attribution of the upstream Hercules project:
https://github.com/zeusssz/hercules-obfuscator
