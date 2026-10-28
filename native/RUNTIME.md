# Bounded native QB executor

`thug2/runtime.hpp` executes a synchronous subset of QB using the shared symbol
snapshot and immutable native values. Token and precompiled script bodies use
the same executor. There are no fixed game addresses, Windows calls, original
allocators or inline assembly in this runtime.

```sh
cmake --build work/native-build
work/native-build/thug2-run goal_add_ped_arrow \
  --load scripts/game/goal_utilities.qb \
  --prx work/english-game/Game/Data/pre/qb_scripts.prx --trace 8
```

This real game function immediately returns; the command is a small execution
smoke check, not a gameplay demonstration. The working x86 game DLL is separate
and remains unchanged.

## Supported behavior

- A script's first line provides default parameters, then entry/call arguments
  override them using typed structure merge rules.
- `name = value` updates local parameters. `<name>` reads a caller/local
  parameter; `<...>` spreads the parameter structure. Missing references are
  omitted in plain argument/value contexts and diagnosed in expressions.
- Named QB calls have independent frames. `return args` adds results to the
  caller's parameters; at the bottom frame its parameters remain as the result.
  Endscript returns without output arguments. Stored code is never modified.
- Compiled `if`/`if not` (`0x47`), `else` (`0x48`), endif and short/long relative
  jumps use checked token-boundary targets. Conditions may be host calls or
  expressions. A non-negated QB-call condition counts as true: the callee runs
  before the caller proceeds into the true body. Negated QB-call conditions
  are explicitly rejected because the legacy cross-frame behavior is not
  modeled. Expression statements test the raw result word (as ExecuteCommand
  does), rather than the arithmetic evaluator's integer/float GetBool rule.
- `begin/repeat` supports nested loops. Repeat arguments are read once, on its
  first visit. With no integer count the loop is unbounded; integer counts are
  decremented as uint32. Thus repeat 0 does not mean zero iterations: it wraps.
  `break` skips the innermost loop, including the repeat arguments.

Loop stacks are frame-local. Original cross-frame loop manipulation, waits,
scheduling, interrupts, events, switch/case, random jumps, object/member dispatch,
resolved function-pointer tokens and engine calls inside expressions are not
implemented. Unsupported cases fault; they are not skipped as successful no-ops.
Unlike the original's permissive missing-command behavior, an unbound command
faults immediately to expose the missing integration.

The value reader now exposes bounded `read` and `arguments` APIs with an explicit
parameter context. Static globals are still evaluated against the final symbol
snapshot; this does not reproduce all original load-time or mutable-reference
semantics. Entry parameters and host outputs must be structures.

## Host boundary

```cpp
thug2::qb::Interpreter vm(symbols);
vm.bind(thug2::name_crc("CheckSomething"), [](const thug2::qb::HostCall& call) {
    // call.name, script, arguments, parameters, depth are read-only context.
    return thug2::qb::HostResult{true, {}};
});
auto result = vm.run(thug2::name_crc("MyScript"));
```

No hosts are registered automatically, including in `thug2-run`. Bindings are
explicit native callbacks keyed by checksum, checked before QB dispatch; a
binding can therefore deliberately override a script name. They return a
condition boolean and optional output parameters to merge into the active
frame. Host exceptions become run faults. Existing side effects of a callback
are not rolled back on a later fault, and fuel cannot preempt a native callback:
hosts must return. Binding changes and reentrant execution during a run are
rejected. Use external synchronization or separate interpreters for threads.

Definitions and compiled code are snapshotted/cached. Each run starts new frames
and parameters; a limit is a stop, **not a resumable yield**. Construct another
interpreter to pick up changed QB definitions. No scheduler or game-state host
implementation is connected by this step.

## Bounds and diagnostics

`RunLimits` defaults: fuel 10,000; call depth 32 (entry included); loop depth 32
per frame; 100,000 compiled tokens; 4 MiB code bytes per function; 256 trace
entries. Token counts are checked before building token metadata. Value parsing
has its separate `EvalLimits`. Parameter merges also cap member counts.

Fuel includes dispatched tokens, statement-boundary scans and charged value
work. It is not the original instruction count or a wall-clock timer; warmed
compiled/value caches can use less work. Traces keep a bounded prefix and mark
truncation. Their offsets are relative to the script body; call depth is shown.

`RunResult` reports Finished, Fault or Limit, root parameters, consumed work,
trace and an error category/message. On faults the root parameters are retained
if the root frame was initialized. Run state is reset even after exceptions.

CLI: `thug2-run SCRIPT --load QB... [--prx ARCHIVE|--dir ROOT] [--int PARAM=VALUE]
[--fuel N] [--trace N]`. Script and parameter identifiers accept names or
`0xCRC`; int arguments must fit int32. Files and sources keep their documented
explicit orders. Exit codes: 0 finished, 1 fault/error, 2 limit. Output is a
status, parameter preview and trace; no engine hosts are installed by the CLI.

## Validation on 2026-10-28

```sh
ctest --test-dir work/native-build --output-on-failure
work/native-build/runtime-corpus work/english-game/Game/Data work/native-validation/runtime.json
.venv/bin/python tools/verify_native_runtime.py
```

Fixtures cover QB-to-QB calls, defaults/overrides, return-result merge, local
expressions, parameter spread, explicit zero-name parameters, if/else, host
conditions, QB-call conditions, cached repeat counts, repeat 0, nested break,
call/fuel/code/trace bounds, host exceptions, source snapshot lifetime and
serialized-image script execution. Release and AddressSanitizer/
UndefinedBehaviorSanitizer runs pass.

The no-host catalog audit attempts all 15,728 **script definitions** in the
565-module inspection snapshot, in CRC order, with empty inputs and 500 fuel
per entry. Caches are shared across entries. Results: **907 finished**, 10
limited; faults: 13,890 missing commands, 144 type faults and 777 unsupported
features. Aliases are not counted as separate entry definitions. Early-return
or empty scripts can finish trivially. These results do not validate complete
gameplay behavior or prove that the remaining scripts are broken.

The optional oracle checks 26 original/native control decisions: 22 repeat
cases (including absent/zero/negative counts) and all four if/not booleans.
Original `ExecuteRepeat` (`0x00472ab0`) uses a pre-initialized loop/count to avoid
game parameter allocators. Original `ExecuteIf` (`0x00475050`) uses a controlled
boolean stub for ExecuteCommand. Native probes run actual QB scenarios with
registered test hosts. This checks those decisions only, **not the full
original interpreter, scheduler or argument parser**.

Only that optional test needs the supported user-owned EXE, existing project
`.venv` and Unicorn outside the macOS execution sandbox. Reports:
`work/native-validation/runtime.json`, `runtime-sanitize.json`,
`runtime-oracle.json` and `runtime-cli.json`. No game assets or DLL are changed.
