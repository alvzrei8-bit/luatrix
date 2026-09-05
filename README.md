# Luatrix

Luatrix is a dependency-free C++17 Lua obfuscator derived from the public Hercules pipeline. It runs the complete compatible transformation pipeline automatically and intentionally exposes one command shape:

```bash
luatrix <input> <out>
```

There are no feature modes, presets, target flags, seeds, or output flags. Luatrix detects Lua, Luau, and GLua from the input and applies every compatible pass. Each run uses fresh randomness.

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

Luatrix currently includes 14 pipeline features:

- Dynamic Code
- Opaque Predicates
- String Encoding
- String To Expressions
- Function Inlining
- Variable Renaming
- Virtual Machine
- Anti Tamper
- Control Flow
- Garbage Code
- Compressor
- Function Wrapping
- Bytecode Encoding
- Watermark (LTRIX)

The lexer protects strings, comments, long-bracket strings, identifiers, and punctuation before transformations are applied. Lua-only VM and bytecode passes are skipped automatically for Luau and GLua inputs.

## Randomized virtual opcodes

The VM does not use a fixed opcode vocabulary. For every output it generates a new private mapping, for example:

```
ABC -> LOAD
QXZ -> NOP
MTR -> RET
```

A later output receives different opcode names. The dispatcher resolves those names at runtime, making static signatures based on a permanent `RET` token ineffective. Explicit deterministic seeds are not exposed by the CLI, so normal runs always receive fresh mappings.

## License and attribution

This native port retains the Apache-2.0 attribution of the upstream Hercules project:
https://github.com/zeusssz/hercules-obfuscator
