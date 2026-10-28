# Bounded native QB executor

`thug2/runtime.hpp` executes a bounded subset of QB using the shared symbol
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
- Compiled `switch/case/default/endswitch` selects the first component of the
  selector and case lines, supports grouped labels and skips nested switches
  while scanning untaken regions. See the switch details below.
- Weighted random, second-stream random, no-repeat and
  permuted branches in statements and runtime values use portable original RNG streams and bounded stored
  histories. See [random jumps](RANDOM.md) for behavior and integration limits.
- Runtime arrays follow separate sizing and population paths, including
  random draws in both passes and nested values; [ARRAYS.md](ARRAYS.md) explains
  capacity/type validation and zero-tail behavior.
- Runtime RandomRange/Range2 use the original type-selection, rounding and
  two RNG streams; [RANDOM.md](RANDOM.md) details boundaries and limitations.
- `begin/repeat` supports nested loops. Repeat arguments are read once, on its
  first visit. With no integer count the loop is unbounded; integer counts are
  decremented as uint32. Thus repeat 0 does not mean zero iterations: it wraps.
  `break` skips the innermost loop, including the repeat arguments.

Loop stacks and waits are frame-local. Original cross-frame loop manipulation,
interrupts, events, object/member dispatch,
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

No hosts are registered automatically. `thug2-run --frames N` opts into Wait. Bindings are
explicit native callbacks keyed by checksum, checked before QB dispatch; a
binding can therefore deliberately override a script name. They return a
condition boolean, optional output parameters to merge into the active
frame, an optional `WaitRequest`, and optional additional fuel work. Host exceptions become run faults. Existing side effects of a callback
are not rolled back on a later fault, and fuel cannot preempt a native callback:
hosts must return. Binding changes and reentrant execution during a run are
rejected. Use external synchronization or separate interpreters for threads.

Definitions and compiled code are snapshotted/cached. Each run starts new frames
and parameters; a limit is a stop, **not a resumable yield**. Construct another
interpreter to pick up changed QB definitions. The explicit task API below preserves continuations across waits. No game-state
host implementation or gameplay scheduler is connected by this step.

## Bounds and diagnostics

`RunLimits` defaults: fuel 10,000; call depth 32 (entry included); loop depth 32
per frame; 100,000 compiled tokens; 4 MiB code bytes per function; 256 trace
entries. Token counts are checked before building token metadata. Value parsing
has its separate `EvalLimits`. Parameter merges also cap member counts.

Fuel includes dispatched tokens, control-flow scans and charged value
work. It is not the original instruction count or a wall-clock timer; warmed
compiled/value caches can use less work. Traces keep a bounded prefix and mark
truncation. Their offsets are relative to the script body; call depth is shown.

`RunResult` reports Finished, Fault, Limit, Ready or Waiting, root parameters, consumed work,
trace and an error category/message. On faults the root parameters are retained
if the root frame was initialized. Run state is reset even after exceptions.

CLI: `thug2-run SCRIPT --load QB... [--prx ARCHIVE|--dir ROOT] [--int PARAM=VALUE]
[--fuel N] [--trace N] [--frames N] [--fps N] [--seed N] [--hosts data]`. Script and parameter identifiers accept names or
`0xCRC`; int arguments must fit int32. Files and sources keep their documented
explicit orders. Exit codes: 0 finished, 1 fault/error, 2 limit, 3 still waiting at the frame cap. Output is a
status, parameter preview and trace. `--frames` enables only Wait and drives
simulated ticks; FPS defaults to 60 (1..1000), frame cap is 1..100000. No sleep
or system clock is used; CLI exit discards its in-process continuation.

## Explicit frame updates and Wait

```cpp
thug2::qb::Interpreter vm(symbols);
vm.install_wait(); // Deliberate override of the Wait checksum.
auto task = vm.start(thug2::name_crc("MyScript"));
auto first = vm.update(task, {0, 1.0/60, 0});
// Waiting keeps PC, call/loop stacks and parameters. Supply the next frame:
auto next = vm.update(task, {1, 1.0/60, 16});
vm.cancel(task); // Also releases completed/faulted tasks.
```

`start` prepares the root frame/defaults without dispatching commands. Its
result starts Ready; preparation errors become Fault/Limit. `inspect` copies
the latest result. `update_all` visits tasks in ascending creation ID order;
this is an explicit deterministic policy, not a reconstruction of the engine's
script-list traversal. Manual `update` order determines priority instead.
There are at most 256 retained tasks, including terminal ones; IDs are never
reused. Interpreter objects cannot be copied or moved. Native callbacks cannot
start, update, cancel tasks or alter bindings reentrantly.

`FrameTick` supplies a monotonic frame ID, finite nonnegative slowed game-frame
seconds, and a wrapping uint32 game-clock time including its offset in ms.
All updates with the same ID must supply identical clocks. A task updates at
most once per frame: repeated calls are idempotent, even after Waiting. Skipped
IDs do not synthesize extra updates. The caller controls clock progression;
there is no implicit sleep, wall clock, background thread or engine integration.

Opt-in Wait follows `CFunc_Wait`, `Wait*` and `UpdateWait` conventions:

- The last unnamed numeric argument wins; integers first convert to float.
  Numeric global names resolve; missing/wrong-type values default to zero.
  Later nonnumeric flags do not erase a preceding numeric match.
- Flag precedence: `seconds`/`second`, `gameframe`/`gameframes`/`game`,
  `frame`/`frames`, then `one_per_frame`. Seconds multiply by 1000, frame
  units by the original float 16.666666. A bare number means milliseconds.
  Values truncate to int64 then retain low 32 bits, including negative wrap.
  Nonfinite/out-of-int64 inputs fault rather than invoking C++ undefined behavior.
- Counter waits count updates, including an immediate check after the Wait
  command. Thus `Wait 0 gameframes` continues immediately, while `Wait 1
  gameframes` yields until the next update.
- Millisecond game timers convert using the original float 0.001 and consume
  supplied slowed frame seconds. A new timer yields at least once. Exact
  equality subtracts to zero and waits until a subsequent positive frame;
  zero-duration frames leave zero timers waiting.
- The unnamed flag CRC `0xc847f2b9` selects the original alternative clock for
  millisecond waits. Its spelling is unverified. RealTimer compares wrapping
  elapsed uint32 ms against a float threshold; zero completes immediately.
  This is the supplied game clock, not necessarily OS wall time.
- `one_per_frame` counts updates, then permits only the first waiter to pass
  per interpreter/frame, including multiple waits within one task. Other
  tasks remain waiting at zero until a later frame. The gate is shared across
  individual `update` and `update_all` calls.

`HostResult::wait` can request the same four wait kinds without QB argument
parsing. `run` faults on a wait request: use start/update to retain a
continuation. A limit remains terminal. Fuel and bounded trace accumulate over
all frames and are never replenished by a wait; each wait check also costs fuel.
Blocked/event/object waits, interrupt execution, spawned engine scripts and
serialization of tasks are not implemented. `thug2/wait.hpp` exposes the pure
wait state operations used by the task runner and differential tests.

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
per entry. Caches are shared across entries. Results: **939 finished**, 40
limited; faults: 14,311 missing commands, 24 type faults and 414 unsupported
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

Wait/resume validation adds zero/exact/paused/wrapping clocks, integer-to-float
conversion, flag precedence and inclusions, nested call/loop continuations,
shared per-frame gates, repeated-tick idempotence, cancellation/capacity,
invalid clocks, nonfinite durations, reentrancy and lifetime fuel bounds. All
15 CTest suites pass in Release and ASan/UBSan builds.

```sh
work/native-build/runtime-corpus /path/to/Game/Data work/wait-corpus.json --wait
/path/to/project/.venv/bin/python tools/verify_native_wait.py \
  --exe /path/to/Game/THUG2PM.exe --report work/wait-oracle.json
```

With only Wait registered, the same 15,728-entry catalog, empty parameters,
500 lifetime fuel and at most four simulated 60 Hz updates per entry gives
940 finished, 102 waiting, 40 limited; faults: 14,201 missing, 24 type,
421 unsupported. Each entry gets a new task, then is canceled. The remaining
waiters are not failures, nor is a finished entry proof of complete gameplay.
Release and ASan/UBSan catalog reports match exactly. The no-host baseline
now gives 939 finished with the fault counts listed above.

The wait oracle checks 444 original setter cases and 2,442 update decisions
(**2,886 checks, zero mismatches**), including unsigned wrap and timer
rounding. Only frame-duration and game-clock providers are stubbed. It does
not validate `CFunc_Wait` argument parsing, clock providers or the original
scheduler. No original file is patched; emulation memory alone is modified.

## Switch dispatch

A switch and each case parse their argument line using the current script's
parameters. Comparison uses the first resulting component, whether named or
unnamed; it does not specifically look for name 0. Empty selectors/cases never
match. Global names resolve against the interpreter snapshot. Integer/float
comparison preserves exact int32 values (16777217 differs from float 16777216),
NaNs do not match, and signed zeroes match. Strings and local strings compare
contents within their own types; pairs/vectors compare coordinates. Mutable
array/structure/script identity comparisons remain explicitly unsupported.

A case is `3e 49 <uint16 next> arguments`, with the next address relative to
its two-byte offset field. A failed comparison scans from that target; a
successful comparison skips newlines and any immediately grouped labels
(`49 <end> 3e 49 <next> arguments`) before entering the common body. Grouped
labels still parse their arguments even after an earlier label matched.
Default skips its `3f 49 <offset>` header and enters the body. Case-body exits
use the existing `49` jump; `3d` endswitch advances normally. Switch dispatch
does not add a runtime stack, so ordinary calls, loops, returns and waits can
execute in a selected body.

Nested switches encountered during a scan are skipped with an iterative
balanced-token scan, including 2,000-deep test input. All scan/lookahead/value
work consumes lifetime fuel; string comparisons charge their text lengths.
All PCs and jump targets must stay on function token boundaries. Missing
endswitch, malformed case/default headers and directly dispatched case/default
labels fault rather than reproducing the original interpreter's infinite loops.

Fixtures cover matching/default/no-default branches, grouped labels, nested
skipped and executed switches, first-component selection, global/missing names,
empty arguments, scalar/string/coordinate equality, unsupported identity,
QB calls, waits, malformed inputs and scan fuel. Release and ASan/UBSan tests
pass. The no-host catalog improves from 907 to 937 finished entries, and the
Wait-only four-frame audit from 908 to 938; this does not imply full gameplay
support for those entries. More entries now reach missing engine commands.

```sh
/path/to/project/.venv/bin/python tools/verify_native_switch.py \
  --exe /path/to/Game/THUG2PM.exe --report work/switch-oracle.json
```

The optional x86 oracle compares **652 switch destinations with zero
mismatches**, including numeric/name/string/coordinate pairs, grouped labels,
empty/default-free selections and nested scans. Original ExecuteSwitch
(0x00473c60), ComponentsEqual (0x004791a0), SkipToken, SkipEndOfLines and
SkipToEndOfSwitch execute. Literal argument parsing and component
allocation/copy/cleanup use controlled stubs (including PC coordinate-object
layout). The native side parses and executes the same QB bodies. This verifies
these dispatch decisions, not the original argument parser, game allocator or
full interpreter. The full Wait-only catalog reports match between Release
and ASan/UBSan builds. Reports are `work/switch-oracle.json`, `switch-no-host.json`,
`switch-wait.json` and `switch-wait-sanitize.json`.

Statement and inline-value random support is described in [RANDOM.md](RANDOM.md). One-shot runs
also consume an internal execution ID for stored-random ownership, so public
task IDs are monotonic but need not be consecutive. Seed and execution order
are part of the random-enabled catalog audit conditions.

Runtime values use an explicit transfer callback and a line-stopping argument
reader. They follow only the selected path, return its actual PC, and retain
original bare-name speculative lookup consumption. Static globals do not
inherit runtime random context. Runtime array control flow uses the original
two-pass behavior with explicit type/capacity bounds; see [ARRAYS.md](ARRAYS.md).

## Opt-in native data commands

`install_data_hosts()` / CLI `--hosts data` registers GetArraySize,
StructureContains and GotParam. They read immutable snapshot data, return conditions and
merge script outputs without an engine. See [HOSTS.md](HOSTS.md) for contracts,
bounds and the first original-QB scenario: 31 profile-name lookups and the
18-of-31 visible-profile count. Objects, events and rendered menus remain
unconnected. `<param> = value` now targets its written checksum even when its
previous value is absent or not a name; ordinary dynamic command names are
still checked separately.

GotParam checks current-frame presence, including zero-valued fields and
flags. Its checksum query uses the original GetChecksum exclusion of unnamed
global structure references. The unmodified menu callback handler now passes
33 callback/forwarding cases; see [HOSTS.md](HOSTS.md) for full-function x86
checks and optional `runtime-corpus --data` / `--data-wait` reports. No-host
and Wait-only catalog modes retain their explicit host selections.
