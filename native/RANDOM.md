# Native QB random jumps

`thug2/random.hpp` implements the original two RNG streams and bounded random
choice histories. The interpreter handles table tokens at statement dispatch
and while parsing runtime values:
`2f` weighted Random, `37` the second stream, `40` RandomNoRepeat and `41`
RandomPermute. Relative table offsets and jump targets use the existing checked
QB document, without original addresses or pointer layouts.

```cpp
thug2::qb::Interpreter vm(symbols);
vm.seed_random(123); // Both streams; defaults to seed 0 in a fresh interpreter.
auto result = vm.run(thug2::name_crc("MyScript"));
```

CLI accepts `--seed N` (uint32 decimal). A new interpreter/process with the same
seed, definition snapshot and execution order reproduces choices. Seed changes
are rejected during a callback. Reseeding resets the two streams, **not** stored
no-repeat/permutation histories, matching InitialRand. Randomness in the host
engine is not connected to these streams.

## Stream behavior

Both streams start with add `0x0aa2fb3f`, multiplier `0x12b9b0a1` and the supplied
seed. Updates retain uint32 wrap and the original arithmetic right shifts. The
sample is the low 16 bits of the new state; range scaling multiplies modulo
2^32 and then arithmetic-shifts by 16. Consequently very large ranges can give
negative samples. This implementation preserves that behavior rather than
substituting a standard C++ distribution. The `RandomStreams` API exposes both
states for differential testing.

Weighted choices sum signed 16-bit weights, draw once from the selected stream,
then subtract weights until the remainder is negative. This step supports
nonnegative weights with a positive total. If scale overflow makes a draw
negative, entry zero is selected just as in the original, even with zero weight.
Empty tables, all-zero weights and negative weights fault explicitly: the
original can scan past its table on such inputs. There is a 65,535-choice cap.

NoRepeat and Permute ignore table weights and use stream one. NoRepeat's first
visit draws an initial last value, then draws a candidate and retries up to 100
times when equal. After that it permits the repeat; a one-choice table therefore
still completes, consuming 102 draws on its first visit. Uint16 choices outside
the table fault instead of reading outside storage.

Permute initializes the order 0..N-1 and performs N*10 swaps, with two random
draws per swap. It increments the cursor **before** choosing: the first visit
returns the second element of the initial order. At wrap, a new shuffle avoids
putting the previous last element first, and that new first element is selected.
Subsequent complete cycles contain all choices exactly once. The supported
count is 2..66, the original inline order capacity. Single-choice and oversized
permutations are rejected because the original accesses invalid/uninitialized
order slots in these cases.

## Ownership and bounds

The interpreter keeps at most 100 stored records, newest insertion first;
creation at capacity evicts the oldest insertion. Lookups do not change age.
Records use compiled script checksum, token offset, opcode, count and execution
owner as native keys. Separate active tasks have separate histories at the
same site. A task retains its history through calls, loops and waits, including
while its terminal result remains retained. Cancel/destruction releases its
records for reuse by later executions. A synchronous run releases ownership
when it ends. A free compatible record is reused before creating another one,
following the original stored-list lookup policy. Script/offset identity is a
portable policy, not the original code-pointer/cache lifetime model.

All executions share the interpreter's RNG streams, so interleaving tasks
changes their draws. Statement table reads, history searches, order creation,
weight scans and every random draw charge lifetime execution fuel. Random
operations cannot replenish fuel. Initialization faults remove the new record;
RNG draws, existing-history mutations and evictions are not rolled back on a
terminal fault. Construct a fresh interpreter for a clean reproduction.

## Runtime value flow

The same transfer resolver now follows tables, long jumps and short jumps in
command/callee arguments, assignments, expressions, script defaults, structure
fields, return arguments, repeat counts and switch/case argument lines. It
resolves the selected path as values are read, rather than scanning to the
first physical EOL before choosing. Argument parsing returns the actual next
PC, so a selected branch can end at a different physical newline or join a
common tail. Tokens and code remain immutable.

`ValueEvaluator::read` accepts an optional runtime `ValueFlow` callback;
`runtime_arguments` stops at a top-level EOL/endscript/EOF/close-paren, unlike
the static bounded `arguments` API. Structures and expressions allow internal
newlines. The interpreter supplies a resolver that checks function boundaries
and charges every transfer and RNG draw. Static globals and the default static
value APIs never inherit this callback or consume random state.

The original's bare-name lookahead is preserved: after a flag, checking for
`=` can resolve a random table, then reset the read position and resolve that
table again on the next argument. Named `name = random(...)` values instead
choose once. Tests check both parameter values and subsequent stream probes;
this is intentional original behavior rather than a cached speculative choice.
Defaults also consume their choices before entry/call parameters override them.
Repeat counts choose only on the first repeat; stored orders survive waits.

Runtime arrays now make the original sizing/type pass and population pass,
including distinct RNG consumption and nested structure/array behavior. See
[ARRAYS.md](ARRAYS.md) for type/capacity checks and remaining null-slot limits.
Random engine host commands, script spawning and cache-dependent owner reuse
remain future work. Runtime RandomRange is described below. Selected statement
branches can call QB, return, change parameters or yield at Wait through the existing runtime. Jump cycles consume lifetime fuel.

## Validation

14 CTest suites pass in Release and ASan/UBSan builds. Random fixtures cover
stream independence, seeds, zero-weight alternatives, no-repeat retries,
permutation cycles/boundaries, task ownership and reuse, pool capacity,
interrupted initialization, malformed tables, jump cycles, repeat loops and
permutation histories across Wait. CLI checks validate seeded reproduction
and reject out-of-range seeds.

```sh
/path/to/project/.venv/bin/python tools/verify_native_random.py \
  --exe /path/to/Game/THUG2PM.exe --report work/random-oracle.json
```

The optional oracle matches **1,800 stream draws and resulting states** plus
**13,800 choice decisions**, with zero mismatches. Original Rnd/Rnd2,
ResolveRandomJumps and CStoredRandom::Shuffle execute unchanged; stored-record
lookup/allocation are controlled stubs for one table/owner. Tests cover six
seeds, both streams, signed/overflowing range scaling, weighted tables and
100 successive choices per stored sequence. This does not validate engine
allocators, ownership lookup, inline-value parsing or the full interpreter.

With seed 0, empty inputs and 500 fuel per entry, the 15,728-entry no-host
catalog gives 939 finished, 40 limited; faults: 14,311 missing, 24 type and
414 unsupported. With only Wait registered and up to four 60 Hz updates per
entry: 940 finished, 102 waiting, 40 limited; faults: 14,201 missing, 24 type,
421 unsupported. More entries now reach a missing engine command or a genuine
wait/fuel stop. The additional parser work changes bounded-audit fuel costs. These are bounded catalog probes, not gameplay
validation. Release and ASan/UBSan Wait-only reports match exactly.

Local reports: `work/random-oracle.json`, `random-no-host.json`,
`random-wait.json`, `random-wait-sanitize.json`, `random-cli.json`. Game assets,
original EXE and reports stay out of Git.

Inline-flow fixtures additionally cover selected/common argument tails,
assignments, expressions, defaults/overrides, QB calls/returns, named structure
fields, selected bare flags, switch/case values, cached random repeat counts,
physical-line boundaries, cycles, static API isolation, runtime arrays and
permutation state across frames. All 14 suites pass under Release and ASan/UBSan;
the full 15,728-entry Wait-only reports match.

```sh
/path/to/project/.venv/bin/python tools/verify_native_inline_random.py \
  --exe /path/to/Game/THUG2PM.exe --report work/inline-random-oracle.json
```

The inline oracle checks **252 scenarios with zero mismatches**: original
ParseArguments and ResolveRandomJumps/Rnd/Rnd2 execute; AddParameter is a
controlled scalar literal-emission stub without engine allocators. It compares
parameters, stop PC and eight subsequent stream probes, across six seeds,
both streams, varied weights, named arguments and bare-name lookahead. It does
not x86-validate expression, structure or array parsing. Previously checked
26 if/repeat decisions and 652 switch destinations still match after parser
integration. New reports: `work/inline-random-oracle.json`,
`inline-random-no-host.json`, `inline-random-wait.json`,
`inline-random-wait-sanitize.json`, `inline-runtime-oracle.json` and
`inline-switch-oracle.json`.

## RandomRange

Runtime value tokens `30` (stream one) and `38` (stream two) require an immediate
pair token containing two float32 endpoints. They work in arguments, returns,
assignments, defaults and structure fields, including structures within arrays.
The original expression parser accepts `30`, but not `38`; Range2 in an
expression faults explicitly. Direct ranges are not literal array elements.
Static globals/default static value APIs do not consume runtime RNG.

Each endpoint adds the original float 0.1 when nonnegative, subtracts it
otherwise, then truncates. If both endpoints are within the original float 1e-5 tolerance
of their int32 candidates, the result is an integer. Reversed endpoints swap,
and the nominal integer interval includes its upper endpoint. Range width,
RNG scaling and result addition preserve original 32-bit wrap, including
unusual results for very large intervals; no signed C++ overflow is used.

Other finite endpoints produce a float: Rnd/Rnd2(0x7ff9), multiply the endpoint
difference by the draw and original float scale bits `0x38000801`, then add
the low endpoint. Intermediates retain x87's 64-bit significand before final
float32 rounding, using portable binary64 error-free transforms. The original
scale can slightly overshoot the nominal high endpoint; the implementation
preserves that rather than clamping. Equal endpoints still consume one draw.
NaN/infinite bounds fault before drawing instead of inheriting masked x87
invalid-conversion behavior. Fuel is charged before the draw.

`ValueRange` is an explicit runtime number-provider callback alongside
`ValueFlow`; the interpreter supplies it from the same RandomSelector streams
used by weighted/permuted jumps. Static materialization never inherits it.
Standalone `RandomSelector::range` returns a numeric type flag and raw bits.

```sh
/path/to/project/.venv/bin/python tools/verify_native_range.py \
  --exe /path/to/Game/THUG2PM.exe --report work/range-oracle.json
```

The optional oracle matches **37,807 range results** (15,001 integer and
22,806 float) plus **75,614 subsequent stream probes**, with zero mismatches.
Original ParseRandomRange (0x0046fbd0), ftol and RNG helpers execute unchanged
at x87 control word 0x037f. Inputs cover seven seeds, tolerance neighbors,
subnormals, signed zero, reversed/equal endpoints, wrapped integer widths,
float extremes and a maximum draw exposing endpoint overshoot. It compares
type, exact result bits and returned PC; nonfinite inputs and the full parser
are outside its x86 scope. Runtime fixtures check operand format, contexts,
stream independence, static isolation, overrides and failure/fuel behavior.

The current no-host/Wait-only counts above include RandomRange and the
reference-assignment correction needed by [real profile scripts](HOSTS.md).
Data hosts are not enabled in those catalog audits. Reports:
`work/range-oracle.json`, `range-no-host.json`, `range-wait.json`,
`range-wait-sanitize.json`, `range-data-cli.json`.
