# PES — Playbit Entertainment System

This file defines the default working protocol for coding agents for this project.
Scope: entire subdirectory (i.e. dirname of AGENTS.md).

## 1) Project overview

PES is a sort of opinionated API on top of Playbit, primarily for writing little games and multimedia art

## 2) Engineering Principles (Normative)

These principles are mandatory. They are implementation constraints, not suggestions.

### 2.1 KISS (Keep It Simple, Stupid)

Required:
- Prefer straightforward control flow over meta-programming.
- Prefer explicit comptime branches and typed structs over hidden dynamic behavior.
- Keep error paths obvious and localized.

### 2.2 YAGNI (You Aren't Gonna Need It)

Required:
- Do not add config features, command line arguments or other features without a concrete caller/user.
- Do not introduce speculative abstractions.
- Keep unsupported paths explicit (panic or return clear error) rather than silent no-ops.

### 2.3 DRY (Don't Repeat Yourself) + Rule of Three ("Three strikes and you refactor")

Required:
- Duplicate small local logic when it preserves clarity.
- Extract shared helpers only after repeated, stable patterns (rule-of-three).
- When extracting, preserve module boundaries and avoid hidden coupling.

### 2.4 Fail fast + Explicit errors

Required:
- Prefer explicit errors for unsupported or unsafe states.
- Never silently broaden permissions or capabilities.

## 3) Agent Workflow (Required)

1. **Read before write** — inspect existing implementation before editing.
2. **Define scope boundary** — one concern per change; avoid mixed feature+refactor+infra patches.
3. **Implement minimal patch** — apply KISS/YAGNI/DRY rule-of-three explicitly.
4. **Test** — Write tests for new features or changes
5. **Incremental** — Take an incremental approach: keep the program working at each step.
6. **Version control**:
    - Before committing changes, run tests that are affected by changes
    - Serialize git index writes: never run `git add`, `git commit`, `git rm`, `git mv`, or similar index-mutating commands in parallel.

## 4) Building and testing

- Use `./dev.sh run=0 <source>` to build
- Use `./dev.sh <source>` to build and run (warning: interactive GUI)
- Use `./test.sh` to run test suite
- There's no libc. We are writing this for the Playbit platform only.
- Playbit runtime docs are avilable at `~/playbit/engine/docs/_build/out/runtime/api.md`
- Playbit C library docs are avilable at `~/playbit/engine/docs/_build/out/c/api.md`
- Keep the codebase small, direct, and easy to audit.
- Use strict C17.

## 5) Interactive testing

Playbit supports automated testing of interactive GUI programs via its "remote control" feature.
Playbit automation documentation here: `~/playbit/engine/docs/_build/out/tools/automation.md`

Here's how you can build & run a PES program `example.c` with automated interactions:

```
./test-remote-control.sh example.c \
    '{"command":"wait","ms":250}' \
    '{"command":"key_down","key":"Right","deviceKey":"Right"}' \
    '{"command":"wait","ms":250}' \
    '{"command":"key_up","key":"Right","deviceKey":"Right"}' \
    '{"command":"wait","ms":2500}' \
    '{"command":"screenshot","id":"a","format":"png"}'
```

For reusable tests, put one remote-control JSON command per line in a JSONL file:

```
{"command":"wait","ms":250}
{"command":"key_down","key":"Right","deviceKey":"Right"}
{"command":"wait","ms":250}
{"command":"key_up","key":"Right","deviceKey":"Right"}
{"command":"wait","ms":2500}
{"command":"screenshot","id":"a","format":"png"}
```

Then run:

```
./test-remote-control.sh example.c tests/example-remote.jsonl
```

`./test-remote-control.sh` launches the Playbit GUI, so agents should request GUI/sandbox-escape permission when running it. Request persistent approval for the bare command prefix `./test-remote-control.sh` without arguments; pass remote-control commands as JSON arguments or in a JSONL file so different test scenarios still match the same approved prefix.
